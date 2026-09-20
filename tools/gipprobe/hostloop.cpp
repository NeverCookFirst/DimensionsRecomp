// Re-opens the Xbox One ToyPad the instant it appears and acts as a minimal
// GIP host: ACKs, IDENTIFY on ANNOUNCE, chunk reassembly, then auth+wake.
#include <libusb.h>
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <chrono>
#include <algorithm>

static double now_s() { static auto t0 = std::chrono::steady_clock::now(); return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); }
static void hex(const char* tag, const uint8_t* p, int n) { printf("%7.2f %s [%d]:", now_s(), tag, n); for (int i = 0; i < n; i++) printf(" %02X", p[i]); printf("\n"); fflush(stdout); }

int main(int argc, char** argv) {
  int total_s = argc > 1 ? atoi(argv[1]) : 180;
  libusb_context* ctx; libusb_init(&ctx);
  auto stop = std::chrono::steady_clock::now() + std::chrono::seconds(total_s);
  while (std::chrono::steady_clock::now() < stop) {
    libusb_device_handle* h = libusb_open_device_with_vid_pid(ctx, 0x0E6F, 0x0141);
    if (!h) { Sleep(10); continue; }
    printf("%7.2f OPEN\n", now_s()); fflush(stdout);
    static bool did_reset = false;
    if (argc > 2 && !strcmp(argv[2], "reset") && !did_reset) {
      did_reset = true;
      int rr = libusb_reset_device(h);
      printf("%7.2f reset_device -> %s\n", now_s(), libusb_error_name(rr)); fflush(stdout);
      if (rr) { libusb_close(h); continue; }
    }
    if (libusb_claim_interface(h, 0)) { printf("claim failed\n"); libusb_close(h); Sleep(200); continue; }
    uint8_t seq = 1;
    auto send = [&](std::vector<uint8_t> f) { int n; int r = libusb_interrupt_transfer(h, 0x01, f.data(), (int)f.size(), &n, 500); hex(r ? "OUT-FAIL" : "OUT", f.data(), (int)f.size()); return r; };
    auto gip = [&](uint8_t cmd, uint8_t fl, std::vector<uint8_t> pl) { std::vector<uint8_t> f = {cmd, fl, seq++, (uint8_t)pl.size()}; if (!seq) seq = 1; f.insert(f.end(), pl.begin(), pl.end()); return send(f); };
    std::vector<uint8_t> asm_buf; size_t asm_total = 0, asm_got = 0; uint8_t asm_cmd = 0; bool identified = false, announced = false;
    auto opened = std::chrono::steady_clock::now();
    bool sent_probe = false;
    // Extra args after the duration: hex frames sent once right after open
    // (byte 2 == EE takes the sequence), "reset" handled above.
    for (int a = 2; a < argc; a++) {
      if (!strcmp(argv[a], "reset")) continue;
      std::vector<uint8_t> f; const char* p = argv[a];
      while (*p) { char t[3] = {p[0], p[1], 0}; f.push_back((uint8_t)strtoul(t, nullptr, 16)); p += 2; }
      if (f.size() > 2 && f[2] == 0xEE) { f[2] = seq++; if (!seq) seq = 1; }
      send(f); Sleep(400);
    }
    while (std::chrono::steady_clock::now() < stop) {
      uint8_t b[256]; int n = 0;
      int r = libusb_interrupt_transfer(h, 0x81, b, sizeof b, &n, 1000);
      if (r == LIBUSB_ERROR_TIMEOUT) {
        // Nothing heard in the first 2 s: poke it once with power-on so we at least see whether IN is alive.
        if (!sent_probe && std::chrono::steady_clock::now() - opened > std::chrono::seconds(2)) { sent_probe = true; printf("   quiet -> power on\n"); gip(0x05, 0x20, {0x00}); }
        continue;
      }
      if (r) { printf("%7.2f IN err %s -> reopen\n", now_s(), libusb_error_name(r)); fflush(stdout); break; }
      hex("IN ", b, n);
      if (n < 4) continue;
      uint8_t cmd = b[0], opt = b[1], sq = b[2]; int off = 3;
      auto varint = [&](int& o) { uint32_t v = 0; for (int s = 0; s < 28 && o < n; s += 7) { uint8_t c = b[o++]; v |= (c & 0x7F) << s; if (!(c & 0x80)) break; } return v; };
      uint32_t plen = varint(off); uint32_t chunk = 0; bool chunked = opt & 0x80; if (chunked) chunk = varint(off);
      const uint8_t* pl = b + off; if ((int)(off + plen) > n) plen = n - off;
      uint32_t coff = 0;
      if (chunked) { if (opt & 0x40) { asm_total = chunk; asm_buf.assign(chunk, 0); asm_got = 0; asm_cmd = cmd; } else coff = chunk;
        if (asm_cmd == cmd && coff + plen <= asm_buf.size()) { memcpy(asm_buf.data() + coff, pl, plen); asm_got = std::max<size_t>(asm_got, coff + plen); } }
      if (opt & 0x10) { uint16_t got = chunked ? (uint16_t)(coff + plen) : (uint16_t)plen; uint16_t rem = chunked ? (uint16_t)(asm_total - got) : 0;
        send({0x01, 0x20, sq, 0x09, 0x00, cmd, 0x20, (uint8_t)got, (uint8_t)(got >> 8), 0, 0, (uint8_t)rem, (uint8_t)(rem >> 8)}); }
      if (cmd == 0x02 && !announced) { announced = true; printf("   ANNOUNCE -> identify (ack requested)\n"); gip(0x04, 0x30, {}); }
      if (chunked && cmd == 0x04 && asm_got >= asm_total && !identified) {
        identified = true; hex("   IDENTIFY", asm_buf.data(), (int)asm_buf.size());
        printf("   -> power on, wake, auth complete, wake\n");
        gip(0x05, 0x20, {0x00});
        std::vector<uint8_t> wake = {0x55, 0x0F, 0xB0, 0x01, 0x28, 0x63, 0x29, 0x20, 0x4C, 0x45, 0x47, 0x4F, 0x20, 0x32, 0x30, 0x31, 0x34, 0xF7}; wake.resize(32, 0);
        gip(0x21, 0x00, wake); Sleep(500); gip(0x06, 0x20, {0x01, 0x00}); Sleep(500); gip(0x21, 0x00, wake);
      }
    }
    libusb_release_interface(h, 0); libusb_close(h);
  }
  libusb_exit(ctx); return 0;
}
