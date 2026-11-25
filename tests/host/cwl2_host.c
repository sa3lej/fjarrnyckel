// The firmware's CWL2 client built for a computer: keys a text to a running cwrecv.py and prints what the
// receiver reports. Used by tests/test_c_client.py to prove the C client and the Python receiver agree.
#include <netdb.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "cwl2.h"

void cwl2_hmac(const uint8_t *key, size_t key_len, const uint8_t *data, size_t len, uint8_t out[32]) {
  unsigned int n = 32;
  HMAC(EVP_sha256(), key, (int)key_len, data, len, out, &n);
}

static uint64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000u + t.tv_nsec / 1000 + 5000000000ull; }

static const char *MORSE[128] = { ['A'] = ".-", ['B'] = "-...", ['C'] = "-.-.", ['D'] = "-..", ['E'] = ".", ['F'] = "..-.", ['G'] = "--.",
  ['H'] = "....", ['I'] = "..", ['J'] = ".---", ['K'] = "-.-", ['L'] = ".-..", ['M'] = "--", ['N'] = "-.", ['O'] = "---", ['P'] = ".--.",
  ['Q'] = "--.-", ['R'] = ".-.", ['S'] = "...", ['T'] = "-", ['U'] = "..-", ['V'] = "...-", ['W'] = ".--", ['X'] = "-..-", ['Y'] = "-.--", ['Z'] = "--.." };

static cwl2_t c;
static int sock;

static void pump(uint64_t until) {       // send what is due and read answers until the given time
  for (;;) {
    uint64_t now = now_us();
    if (cwl2_due(&c, now)) { uint8_t pkt[CWL2_MAX_PACKET]; size_t n = cwl2_packet(&c, now, pkt); send(sock, pkt, n, 0); }
    uint8_t buf[256]; ssize_t n;
    while ((n = recv(sock, buf, sizeof buf, MSG_DONTWAIT)) > 0) cwl2_on_status(&c, buf, (size_t)n, now_us());
    if (now >= until) return;
    struct pollfd fd = { sock, POLLIN, 0 }; poll(&fd, 1, 1);
  }
}

static void key(bool down) {
  uint64_t now = now_us();
  cwl2_edge(&c, now, down);
  uint8_t pkt[CWL2_MAX_PACKET]; size_t n = cwl2_packet(&c, now, pkt); send(sock, pkt, n, 0);
}

int main(int argc, char **argv) {
  if (argc < 6) { fprintf(stderr, "usage: cwl2_host KEYFILE HOST PORT WPM TEXT [BUFFER_MS]\n"); return 2; }
  uint8_t keybuf[96]; FILE *f = fopen(argv[1], "rb"); if (!f) { perror("key"); return 2; }
  size_t klen = fread(keybuf, 1, sizeof keybuf, f); fclose(f);
  while (klen && (keybuf[klen - 1] == '\n' || keybuf[klen - 1] == '\r' || keybuf[klen - 1] == ' ')) klen--;
  struct addrinfo hints = { .ai_socktype = SOCK_DGRAM }, *res;
  if (getaddrinfo(argv[2], argv[3], &hints, &res)) { fprintf(stderr, "cannot resolve\n"); return 2; }
  sock = socket(res->ai_family, SOCK_DGRAM, 0); connect(sock, res->ai_addr, res->ai_addrlen);
  srand((unsigned)now_us());
  cwl2_init(&c, keybuf, klen, (uint32_t)rand() | 1);
  int wpm = atoi(argv[4]); c.wpm = (uint8_t)wpm;
  if (argc > 6) c.delay_ms = (uint16_t)atoi(argv[6]);
  uint64_t unit = 1200000u / (unsigned)wpm, give_up = now_us() + 5000000;
  while (!cwl2_linked(&c)) { if (now_us() > give_up) { printf("{\"linked\":false}\n"); return 1; } pump(now_us() + 20000); }
  uint64_t at = now_us() + 200000; int edges = 0;
  for (const char *ch = argv[5]; *ch; ch++) {
    if (*ch == ' ') { at += unit * 4; continue; }
    const char *code = MORSE[(unsigned char)*ch & 127]; if (!code) continue;
    for (; *code; code++) { pump(at); key(true); at += unit * (*code == '.' ? 1 : 3); pump(at); key(false); at += unit; edges += 2; }
    at += unit * 2;
  }
  pump(now_us() + 1000000);
  printf("{\"linked\":true,\"edges\":%d,\"rx_pkts\":%u,\"late\":%u,\"lost\":%u,\"delay_ms\":%u,\"receiver\":%u,\"flags\":%u,\"rtt_us\":%lld}\n",
         edges, c.status.rx_pkts, c.status.late, c.status.lost, c.status.delay_ms, c.status.receiver, c.status.flags, (long long)c.rtt_us);
  return 0;
}
