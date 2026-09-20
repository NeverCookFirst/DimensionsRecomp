// Probe for the Xbox One LEGO Dimensions ToyPad (0E6F:0141, GIP).
// Dumps descriptors, sends GIP power-on (+ optional metadata request),
// then prints every frame the pad sends for a few seconds.
#include <libusb.h>
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <chrono>

static double now_s() {
  static auto t0 = std::chrono::steady_clock::now();
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}
static void hex(const char* tag, const uint8_t* p, int n) {
  printf("%7.2f %s [%d]:", now_s(), tag, n);
  for (int i = 0; i < n; i++) printf(" %02X", p[i]);
  printf("\n");
  fflush(stdout);
}

static uint8_t g_seq = 1;
static libusb_device_handle* h;
static uint8_t ep_in, ep_out;

static int send(std::vector<uint8_t> f) {
  int n = 0;
  int r = libusb_interrupt_transfer(h, ep_out, f.data(), (int)f.size(), &n, 500);
  hex(r == 0 ? "OUT" : "OUT-FAIL", f.data(), (int)f.size());
  if (r) printf("  err %s\n", libusb_error_name(r));
  return r;
}
static int gip(uint8_t cmd, uint8_t flags, std::vector<uint8_t> payload) {
  std::vector<uint8_t> f = {cmd, flags, g_seq++, (uint8_t)payload.size()};
  f.insert(f.end(), payload.begin(), payload.end());
  return send(f);
}
static void drain(int ms) {
  auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (std::chrono::steady_clock::now() < end) {
    uint8_t b[256]; int n = 0;
    int left = (int)std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()).count(); if (left < 1) break;
    int r = libusb_interrupt_transfer(h, ep_in, b, sizeof b, &n, left);
    if (r == 0) hex("IN ", b, n);
    else if (r != LIBUSB_ERROR_TIMEOUT) { printf("%7.2f IN err %s\n", now_s(), libusb_error_name(r)); fflush(stdout); break; }
  }
}

int main(int argc, char** argv) {
  libusb_context* ctx;
  if (libusb_init(&ctx)) return 1;
  printf("waiting for 0E6F:0141...\n"); fflush(stdout);
  for (int t = 0; t < 600 && !h; t++) { h = libusb_open_device_with_vid_pid(ctx, 0x0E6F, 0x0141); if (!h) Sleep(100); }
  if (!h) { printf("no 0E6F:0141\n"); return 1; }
  libusb_device* d = libusb_get_device(h);
  libusb_device_descriptor dd; libusb_get_device_descriptor(d, &dd);
  printf("dev %04X:%04X bcd %04X cfgs %d\n", dd.idVendor, dd.idProduct, dd.bcdDevice, dd.bNumConfigurations);
  unsigned char s[128];
  if (libusb_get_string_descriptor_ascii(h, dd.iProduct, s, sizeof s) > 0) printf("product: %s\n", s);
  if (libusb_get_string_descriptor_ascii(h, dd.iManufacturer, s, sizeof s) > 0) printf("mfr: %s\n", s);
  if (libusb_get_string_descriptor_ascii(h, dd.iSerialNumber, s, sizeof s) > 0) printf("serial: %s\n", s);
  libusb_config_descriptor* c;
  libusb_get_active_config_descriptor(d, &c);
  int iface = -1;
  for (int i = 0; i < c->bNumInterfaces; i++)
    for (int a = 0; a < c->interface[i].num_altsetting; a++) {
      auto& alt = c->interface[i].altsetting[a];
      printf("iface %d alt %d class %02X/%02X/%02X eps %d\n", alt.bInterfaceNumber, alt.bAlternateSetting,
             alt.bInterfaceClass, alt.bInterfaceSubClass, alt.bInterfaceProtocol, alt.bNumEndpoints);
      for (int e = 0; e < alt.bNumEndpoints; e++) {
        auto& ep = alt.endpoint[e];
        printf("   ep %02X attr %02X maxpkt %d interval %d\n", ep.bEndpointAddress, ep.bmAttributes, ep.wMaxPacketSize, ep.bInterval);
        if (a == 0 && (ep.bmAttributes & 3) == LIBUSB_TRANSFER_TYPE_INTERRUPT) {
          if (ep.bEndpointAddress & 0x80) { if (!ep_in) { ep_in = ep.bEndpointAddress; iface = alt.bInterfaceNumber; } }
          else if (!ep_out) ep_out = ep.bEndpointAddress;
        }
      }
    }
  libusb_free_config_descriptor(c);
  printf("using iface %d in %02X out %02X\n", iface, ep_in, ep_out);
  int r = libusb_claim_interface(h, iface);
  if (r) { printf("claim failed %s\n", libusb_error_name(r)); return 1; }

  // Args: "listen:<ms>", "power", "meta", or raw hex frames (byte 2 == EE
  // gets our sequence number); each raw frame is followed by a 1.5s read.
  for (int i = 1; i < argc; i++) {
    if (!strncmp(argv[i], "listen:", 7)) { printf("--- listen %s\n", argv[i] + 7); drain(atoi(argv[i] + 7)); continue; }
    if (!strncmp(argv[i], "host:", 5)) {
      // Minimal GIP host: ACK anything that asks, request IDENTIFY on
      // ANNOUNCE, reassemble chunks, then auth-complete + wake once identified.
      int ms = atoi(argv[i] + 5); printf("--- host %d ms\n", ms); fflush(stdout);
      auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
      std::vector<uint8_t> asm_buf; size_t asm_total = 0, asm_got = 0; uint8_t asm_cmd = 0;
      bool identified = false;
      auto sendgip = [&](uint8_t cmd, uint8_t fl, std::vector<uint8_t> pl) { gip(cmd, fl, pl); if (!g_seq) g_seq = 1; };
      while (std::chrono::steady_clock::now() < end) {
        uint8_t b[256]; int n = 0;
        int left = (int)std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()).count(); if (left < 1) break;
        int r = libusb_interrupt_transfer(h, ep_in, b, sizeof b, &n, left);
        if (r == LIBUSB_ERROR_TIMEOUT) continue;
        if (r) { printf("%7.2f IN err %s\n", now_s(), libusb_error_name(r)); break; }
        hex("IN ", b, n);
        if (n < 4) continue;
        uint8_t cmd = b[0], opt = b[1], seq = b[2]; int off = 3;
        auto varint = [&](int& o) { uint32_t v = 0; for (int s = 0; s < 28 && o < n; s += 7) { uint8_t c = b[o++]; v |= (c & 0x7F) << s; if (!(c & 0x80)) break; } return v; };
        uint32_t plen = varint(off); uint32_t chunk = 0; bool chunked = opt & 0x80;
        if (chunked) chunk = varint(off);
        const uint8_t* pl = b + off; if ((int)(off + plen) > n) plen = n - off;
        uint32_t coff = 0;
        if (chunked) { if (opt & 0x40) { asm_total = chunk; asm_buf.assign(chunk, 0); asm_got = 0; asm_cmd = cmd; coff = 0; } else coff = chunk;
          if (asm_cmd == cmd && coff + plen <= asm_buf.size()) { std::memcpy(asm_buf.data() + coff, pl, plen); asm_got = std::max<size_t>(asm_got, coff + plen); } }
        if (opt & 0x10) {
          uint16_t got = chunked ? (uint16_t)(coff + plen) : (uint16_t)plen;
          uint16_t rem = chunked ? (uint16_t)(asm_total - got) : 0;
          std::vector<uint8_t> ack = {0x01, 0x20, seq, 0x09, 0x00, cmd, 0x20, (uint8_t)got, (uint8_t)(got >> 8), 0, 0, (uint8_t)rem, (uint8_t)(rem >> 8)};
          send(ack);
        }
        if (cmd == 0x02) { printf("   ANNOUNCE -> identify\n"); sendgip(0x04, 0x20, {}); }
        if (chunked && cmd == 0x04 && asm_got >= asm_total && !identified) {
          identified = true; hex("   IDENTIFY", asm_buf.data(), (int)asm_buf.size());
          printf("   -> power on, auth complete, wake\n");
          sendgip(0x05, 0x20, {0x00});
          std::vector<uint8_t> wake = {0x55, 0x0F, 0xB0, 0x01, 0x28, 0x63, 0x29, 0x20, 0x4C, 0x45, 0x47, 0x4F, 0x20, 0x32, 0x30, 0x31, 0x34, 0xF7}; wake.resize(32, 0);
          sendgip(0x21, 0x00, wake);
          sendgip(0x06, 0x20, {0x01, 0x00});
          sendgip(0x21, 0x00, wake);
        }
      }
      continue;
    }
    if (!strcmp(argv[i], "init")) {
      // Documented Xbox One init: wake in 0x21; no reply -> auth 06 01 00 -> wake.
      std::vector<uint8_t> wake = {0x21, 0x00, 0, 0x20, 0x55, 0x0F, 0xB0, 0x01, 0x28, 0x63, 0x29, 0x20, 0x4C, 0x45, 0x47, 0x4F, 0x20, 0x32, 0x30, 0x31, 0x34, 0xF7};
      wake.resize(36, 0);
      for (int round = 0; round < 6; round++) {
        printf("--- init round %d\n", round);
        for (int k = 0; k < 2; k++) {
          std::vector<uint8_t> w = wake; w[2] = g_seq++; if (!g_seq) g_seq = 1;
          if (send(w)) { libusb_clear_halt(h, ep_out); Sleep(200); w[2] = g_seq++; send(w); }
          drain(1000);
        }
        std::vector<uint8_t> auth = {0x06, 0x20, g_seq++, 0x02, 0x01, 0x00};
        if (send(auth)) { libusb_clear_halt(h, ep_out); Sleep(200); auth[2] = g_seq++; send(auth); }
        drain(1500);
      }
      continue;
    }
    if (!strcmp(argv[i], "power")) { printf("--- power on\n"); gip(0x05, 0x20, {0x00}); drain(1000); continue; }
    if (!strcmp(argv[i], "meta")) { printf("--- metadata request\n"); gip(0x04, 0x20, {}); drain(1500); continue; }
    std::vector<uint8_t> f; const char* p = argv[i];
    while (*p) { if (*p == ' ' || *p == ',') { p++; continue; } f.push_back((uint8_t)strtoul(std::string(p, 2).c_str(), nullptr, 16)); p += 2; }
    printf("--- raw %s\n", argv[i]);
    // "seq" placeholder: byte index 2 gets our seq if frame starts with a GIP cmd and byte 2 is 0xEE
    if (f.size() > 2 && f[2] == 0xEE) { f[2] = g_seq++; if (!g_seq) g_seq = 1; }
    send(f); drain(1500);
  }
  printf("--- final idle 2s\n"); drain(2000);
  libusb_release_interface(h, iface); libusb_close(h); libusb_exit(ctx);
  return 0;
}
