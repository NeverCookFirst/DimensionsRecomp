// Opt-in sampling profiler (LEGO_SAMPLE_PROFILE=1). F9 starts recording, F9
// again stops it and writes profile-<tick>.bin next to the exe. Every ~1 ms
// each other thread is suspended, its stack is unwound with the x64 unwind
// tables (up to 24 frames) and resumed. Addresses are resolved offline against
// legodimensions.map; the file header lists the loaded modules.
#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <timeapi.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "winmm.lib")

namespace {

constexpr int kMaxFrames = 24;

struct Sample {
  uint32_t tid;
  uint32_t depth;
  uint64_t frames[kMaxFrames];
};

void WriteProfile(const std::vector<Sample>& samples) {
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  std::wstring path(exe);
  path = path.substr(0, path.find_last_of(L"\\/") + 1) + L"profile-" +
         std::to_wstring(GetTickCount64()) + L".bin";
  FILE* f = _wfopen(path.c_str(), L"wb");
  if (!f) return;
  HMODULE modules[1024];
  DWORD needed = 0;
  EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed);
  const uint32_t count = needed / sizeof(HMODULE);
  std::fwrite("LSP1", 1, 4, f);
  std::fwrite(&count, 4, 1, f);
  for (uint32_t i = 0; i < count; ++i) {
    MODULEINFO info{};
    GetModuleInformation(GetCurrentProcess(), modules[i], &info, sizeof(info));
    char name[MAX_PATH] = {};
    GetModuleBaseNameA(GetCurrentProcess(), modules[i], name, MAX_PATH);
    const uint64_t base = reinterpret_cast<uint64_t>(info.lpBaseOfDll);
    const uint32_t size = info.SizeOfImage;
    const uint32_t length = uint32_t(std::strlen(name));
    std::fwrite(&base, 8, 1, f);
    std::fwrite(&size, 4, 1, f);
    std::fwrite(&length, 4, 1, f);
    std::fwrite(name, 1, length, f);
  }
  const uint64_t total = samples.size();
  std::fwrite(&total, 8, 1, f);
  for (const auto& s : samples) {
    std::fwrite(&s.tid, 4, 1, f);
    std::fwrite(&s.depth, 4, 1, f);
    std::fwrite(s.frames, 8, s.depth, f);
  }
  std::fclose(f);
  Beep(880, 150);
}

// Unwind a suspended thread from its captured context. Reads only the stack
// and the static unwind tables, and never allocates.
uint32_t Unwind(CONTEXT context, uint64_t* frames) {
  uint32_t depth = 0;
  while (depth < kMaxFrames && context.Rip) {
    frames[depth++] = context.Rip;
    DWORD64 image_base = 0;
    auto* entry = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
    if (!entry) {
      if (IsBadReadPtr(reinterpret_cast<void*>(context.Rsp), 8)) break;
      context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
      context.Rsp += 8;
      continue;
    }
    void* handler_data = nullptr;
    DWORD64 establisher = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, entry, &context,
                     &handler_data, &establisher, nullptr);
  }
  return depth;
}

void SamplerThread() {
  const DWORD self = GetCurrentThreadId();
  const DWORD pid = GetCurrentProcessId();
  std::vector<Sample> samples;
  bool recording = false;
  bool key_was_down = false;
  const char* trigger = std::getenv("LEGO_SAMPLE_PROFILE_TRIGGER");
  timeBeginPeriod(1);
  for (;;) {
    const bool key_down = trigger ? GetFileAttributesA(trigger) != INVALID_FILE_ATTRIBUTES
                                 : (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (trigger && recording && !key_down && key_was_down) {
      WriteProfile(samples);
      samples.clear();
      recording = false;
    }
    if (key_down && !key_was_down) {
      if (recording) {
        WriteProfile(samples);
        samples.clear();
        samples.shrink_to_fit();
      } else {
        samples.reserve(1u << 20);
        Beep(440, 150);
      }
      recording = !recording;
    }
    key_was_down = key_down;
    if (!recording) {
      Sleep(50);
      continue;
    }
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
      if (entry.th32OwnerProcessID != pid || entry.th32ThreadID == self) continue;
      HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                 THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
      if (!thread) continue;
      Sample sample{};
      bool captured = false;
      if (SuspendThread(thread) != DWORD(-1)) {
        CONTEXT context{};
        context.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(thread, &context)) {
          sample.tid = entry.th32ThreadID;
          sample.depth = Unwind(context, sample.frames);
          captured = true;
        }
        ResumeThread(thread);
      }
      CloseHandle(thread);
      // Allocate only after the thread runs again: it may hold the heap lock.
      if (captured) samples.push_back(sample);
    }
    CloseHandle(snapshot);
    Sleep(1);
  }
}

struct AutoStart {
  AutoStart() {
    if (!std::getenv("LEGO_SAMPLE_PROFILE")) return;
    std::thread(SamplerThread).detach();
  }
} g_auto_start;

}  // namespace
#endif  // _WIN32
