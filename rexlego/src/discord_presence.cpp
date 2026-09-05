// legodimensions - ReXGlue Recompiled Project
//
// Discord Rich Presence over the client's local IPC pipe. See
// discord_presence.h for why this is hand-rolled rather than a library.
//
// The protocol is small: connect to \\.\pipe\discord-ipc-N (N in 0..9, one per
// running client), then exchange length-prefixed frames of
//   [uint32 opcode][uint32 payload length][utf-8 json payload]
// with opcode 0 = HANDSHAKE, 1 = FRAME, 2 = CLOSE, 3 = PING, 4 = PONG.
// A handshake carrying the application id is followed by a SET_ACTIVITY frame,
// and the client keeps showing that activity until the pipe closes.

#include "discord_presence.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>

#if defined(_WIN32)
// WIN32_LEAN_AND_MEAN comes from the SDK's compile definitions; defining it
// here again only earns a -Wmacro-redefined warning.
#include <windows.h>
#endif

// The application id from the Discord developer portal. It is what ties the
// presence to the game profile at https://discord.com/games/<id>, so clicking
// the activity in a profile lands on our page; the game NAME shown in Discord
// is the application's name there, not anything we send.
REXCVAR_DEFINE_STRING(discord_app_id, "1442727526052007966", "Discord",
                      "Discord application id used for Rich Presence");
REXCVAR_DEFINE_BOOL(discord_rpc, true, "Discord",
                    "Publish Rich Presence to a running Discord client");
REXCVAR_DEFINE_STRING(discord_details, "Xbox 360 recompilation", "Discord",
                      "First presence line (activity details)");
REXCVAR_DEFINE_STRING(discord_state, "In-game", "Discord",
                      "Second presence line (activity state)");
// Normally this would name an art asset uploaded in the developer portal, but
// the application above is not ours to upload to, so we hand Discord a URL
// instead and it proxies the image itself. Hosted in our own public repo
// because Discord fetches this server-side: a private repo's raw links carry
// an expiring token and would break within the hour.
REXCVAR_DEFINE_STRING(
    discord_large_image,
    "https://raw.githubusercontent.com/NeverCookFirst/Xenia-Seamless-Toypad-Build/"
    "toypad/assets/discord_cover.png",
    "Discord", "Large image: portal art asset name, or an image URL");
REXCVAR_DEFINE_STRING(discord_large_text, "LEGO Dimensions", "Discord",
                      "Tooltip shown when hovering the large image");
// Buttons are visible to everyone EXCEPT the player themselves - Discord hides
// them in your own profile, which is not a bug on our side. Blank the label to
// drop the button entirely.
REXCVAR_DEFINE_STRING(discord_button_label, "Game page", "Discord",
                      "Presence button label (empty = no button)");
REXCVAR_DEFINE_STRING(discord_button_url,
                      "https://discord.com/games/1442727526052007966", "Discord",
                      "URL the presence button opens");

namespace legodimensions::discord {
namespace {

enum Opcode : uint32_t {
  kHandshake = 0,
  kFrame = 1,
  kClose = 2,
  kPing = 3,
  kPong = 4,
};

// Minimal JSON string escaping. The values come from cvars, so they are short
// and only ever authored by the user editing their own toml.
std::string JsonEscape(std::string_view in) {
  std::string out;
  out.reserve(in.size() + 8);
  for (char c : in) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          out += fmt::format("\\u{:04x}", static_cast<unsigned char>(c));
        } else {
          out += c;
        }
    }
  }
  return out;
}

std::string BuildActivityPayload(int64_t start_timestamp, uint32_t pid) {
  std::string activity =
      fmt::format(R"("type":0,"details":"{}","state":"{}","timestamps":{{"start":{}}})",
                  JsonEscape(REXCVAR_GET(discord_details)),
                  JsonEscape(REXCVAR_GET(discord_state)), start_timestamp);

  const std::string large_image = REXCVAR_GET(discord_large_image);
  if (!large_image.empty()) {
    activity += fmt::format(R"(,"assets":{{"large_image":"{}","large_text":"{}"}})",
                            JsonEscape(large_image),
                            JsonEscape(REXCVAR_GET(discord_large_text)));
  }

  const std::string button_label = REXCVAR_GET(discord_button_label);
  const std::string button_url = REXCVAR_GET(discord_button_url);
  if (!button_label.empty() && !button_url.empty()) {
    activity += fmt::format(R"(,"buttons":[{{"label":"{}","url":"{}"}}])",
                            JsonEscape(button_label), JsonEscape(button_url));
  }

  return fmt::format(
      R"({{"cmd":"SET_ACTIVITY","nonce":"legodimensions-1","args":{{"pid":{},"activity":{{{}}}}}}})",
      pid, activity);
}

#if defined(_WIN32)

class IpcConnection {
 public:
  ~IpcConnection() { Disconnect(); }

  bool connected() const { return pipe_ != INVALID_HANDLE_VALUE; }

  // Walks the ten possible pipes; more than one exists when several Discord
  // builds (stable/PTB/canary) run side by side, and the first that answers is
  // as good as any.
  bool Connect() {
    for (int i = 0; i < 10; ++i) {
      std::string name = fmt::format(R"(\\.\pipe\discord-ipc-{})", i);
      HANDLE h = CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                             OPEN_EXISTING, 0, nullptr);
      if (h != INVALID_HANDLE_VALUE) {
        pipe_ = h;
        return true;
      }
    }
    return false;
  }

  void Disconnect() {
    if (pipe_ != INVALID_HANDLE_VALUE) {
      CloseHandle(pipe_);
      pipe_ = INVALID_HANDLE_VALUE;
    }
  }

  bool WriteFrame(Opcode op, const std::string& payload) {
    if (!connected()) {
      return false;
    }
    std::vector<char> frame(8 + payload.size());
    uint32_t opcode = static_cast<uint32_t>(op);
    uint32_t length = static_cast<uint32_t>(payload.size());
    std::memcpy(frame.data(), &opcode, 4);
    std::memcpy(frame.data() + 4, &length, 4);
    std::memcpy(frame.data() + 8, payload.data(), payload.size());

    DWORD written = 0;
    if (!WriteFile(pipe_, frame.data(), static_cast<DWORD>(frame.size()), &written,
                   nullptr) ||
        written != frame.size()) {
      Disconnect();
      return false;
    }
    return true;
  }

  // Reads one frame, waiting up to timeout_ms for it to show up. Returns false
  // on timeout, leaving the connection intact; a broken pipe disconnects.
  //
  // The pipe is opened in blocking mode, so peek before every read and only
  // take what is already buffered - a plain ReadFile would park this thread
  // forever on a client that has nothing to say.
  bool ReadFrame(uint32_t& opcode, std::string& payload, int timeout_ms) {
    constexpr int kPollIntervalMs = 50;
    for (int waited = 0;; waited += kPollIntervalMs) {
      if (!connected()) {
        return false;
      }
      DWORD available = 0;
      if (!PeekNamedPipe(pipe_, nullptr, 0, nullptr, &available, nullptr)) {
        Disconnect();
        return false;
      }
      if (available >= 8) {
        break;
      }
      if (waited >= timeout_ms) {
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
    }

    char header[8];
    DWORD read = 0;
    if (!ReadFile(pipe_, header, 8, &read, nullptr) || read != 8) {
      Disconnect();
      return false;
    }
    uint32_t length = 0;
    std::memcpy(&opcode, header, 4);
    std::memcpy(&length, header + 4, 4);

    payload.assign(length, '\0');
    for (uint32_t offset = 0; offset < length;) {
      if (!ReadFile(pipe_, payload.data() + offset, length - offset, &read, nullptr) ||
          read == 0) {
        Disconnect();
        return false;
      }
      offset += read;
    }
    return true;
  }

  // Drains whatever the client has queued and answers its keepalives.
  void Pump() {
    uint32_t opcode = 0;
    std::string payload;
    while (ReadFrame(opcode, payload, 0)) {
      if (opcode == kPing) {
        WriteFrame(kPong, payload);
      } else if (opcode == kClose) {
        REXLOG_INFO("Discord Rich Presence closed by the client: {}", payload);
        Disconnect();
        return;
      }
      // Anything else is a command reply - an ack for an activity we already
      // sent. Errors are reported where the activity is sent, not here.
    }
  }

 private:
  HANDLE pipe_ = INVALID_HANDLE_VALUE;
};

std::atomic<bool> g_running{false};
std::thread g_thread;

// Handshake, then the activity. Returns true once Discord has acknowledged the
// activity. On any failure the connection is dropped so the caller retries from
// scratch on its next tick.
//
// The wait for READY is the whole point of doing this in two steps: Discord
// silently discards commands that arrive before it has answered the handshake,
// so firing SET_ACTIVITY straight after the handshake write looks perfectly
// healthy from our side and shows nothing in the client.
bool Announce(IpcConnection& connection, int64_t start_timestamp, uint32_t pid,
              bool already_announced) {
  const std::string handshake = fmt::format(R"({{"v":1,"client_id":"{}"}})",
                                            JsonEscape(REXCVAR_GET(discord_app_id)));
  if (!connection.WriteFrame(kHandshake, handshake)) {
    return false;
  }

  uint32_t opcode = 0;
  std::string payload;
  if (!connection.ReadFrame(opcode, payload, 5000) ||
      payload.find(R"("evt":"READY")") == std::string::npos) {
    REXLOG_WARN("Discord Rich Presence handshake was not acknowledged: {}",
                   payload.empty() ? "no reply" : payload);
    connection.Disconnect();
    return false;
  }

  if (!connection.WriteFrame(kFrame, BuildActivityPayload(start_timestamp, pid))) {
    return false;
  }

  // Discord reports a rejected field (a malformed asset, an over-long label) in
  // the reply rather than by closing the pipe, so surface it instead of leaving
  // a silently empty presence.
  if (connection.ReadFrame(opcode, payload, 5000) &&
      payload.find(R"("evt":"ERROR")") != std::string::npos) {
    REXLOG_WARN("Discord rejected the presence: {}", payload);
    connection.Disconnect();
    return false;
  }

  if (!already_announced) {
    REXLOG_INFO("Discord Rich Presence live (app id {})", REXCVAR_GET(discord_app_id));
  }
  return true;
}

void ThreadMain() {
  const int64_t start_timestamp = static_cast<int64_t>(std::time(nullptr));
  const uint32_t pid = static_cast<uint32_t>(GetCurrentProcessId());

  IpcConnection connection;
  bool announced = false;

  while (g_running.load(std::memory_order_relaxed)) {
    if (!connection.connected()) {
      if (connection.Connect() && Announce(connection, start_timestamp, pid, announced)) {
        announced = true;
      }
    } else {
      connection.Pump();
    }

    // Reconnect attempts and keepalive pumping both sit at 2 s, sliced so
    // Stop() does not wait out a long sleep. Discord rate limits presence
    // updates to one per 15 s, but we only ever send one, on connect.
    for (int i = 0; i < 20 && g_running.load(std::memory_order_relaxed); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }
}

#endif  // _WIN32

}  // namespace

#if defined(_WIN32)

void Start() {
  if (!REXCVAR_GET(discord_rpc) || g_running.load(std::memory_order_relaxed)) {
    return;
  }
  g_running.store(true, std::memory_order_relaxed);
  g_thread = std::thread(ThreadMain);
}

void Stop() {
  if (!g_running.exchange(false, std::memory_order_relaxed)) {
    return;
  }
  if (g_thread.joinable()) {
    g_thread.join();
  }
}

#else

// Discord's IPC is a unix domain socket at $XDG_RUNTIME_DIR/discord-ipc-N on
// other platforms. Not wired up: this project only ships a Windows build.
void Start() {}
void Stop() {}

#endif  // _WIN32

}  // namespace legodimensions::discord
