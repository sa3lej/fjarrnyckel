#include <string.h>
#include "cwl2.h"

#define REPEAT_US 500000u            // every packet repeats the edges of this long
#define ACTIVE_US 2000000u           // ACTIVE for this long after an edge
#define HEARTBEAT_ACTIVE_US 20000u
#define HEARTBEAT_IDLE_US 100000u
static const uint32_t EDGE_REPEATS_US[2] = { 3000, 8000 };   // an edge packet is sent again this long after the edge
#define MAX_EDGES 8
#define TAG_LEN 16
#define CAP (sizeof ((cwl2_t *)0)->edges / sizeof ((cwl2_t *)0)->edges[0])

static void put(uint8_t **p, uint64_t v, int bytes) { for (int i = bytes - 1; i >= 0; i--) *(*p)++ = (uint8_t)(v >> (8 * i)); }
static uint64_t get(const uint8_t **p, int bytes) { uint64_t v = 0; while (bytes--) v = (v << 8) | *(*p)++; return v; }

void cwl2_init(cwl2_t *c, const uint8_t *key, size_t key_len, uint32_t session) {
  memset(c, 0, sizeof *c);
  if (key_len > sizeof c->key) key_len = sizeof c->key;
  memcpy(c->key, key, key_len); c->key_len = key_len;
  c->session = session ? session : 1;
  c->rtt_us = -1;
}

static void drop_oldest(cwl2_t *c) { memmove(&c->edges[0], &c->edges[1], (--c->n_edges) * sizeof c->edges[0]); }

void cwl2_edge(cwl2_t *c, uint64_t t_us, bool down) {
  if (c->n_edges == CAP) drop_oldest(c);
  c->edges[c->n_edges].index = c->next_index++;
  c->edges[c->n_edges].t = t_us;
  c->edges[c->n_edges].down = down;
  c->n_edges++;
  c->key_down = down; c->last_edge = t_us; c->has_edge = true;
  c->repeats[0] = t_us + EDGE_REPEATS_US[0]; c->repeats[1] = t_us + EDGE_REPEATS_US[1]; c->n_repeats = 2;
}

static bool active(const cwl2_t *c, uint64_t now) { return c->has_edge && now - c->last_edge < ACTIVE_US; }

bool cwl2_due(cwl2_t *c, uint64_t now) {
  if (!c->has_sent) return true;
  if (c->n_repeats && now >= c->repeats[0]) { c->repeats[0] = c->repeats[1]; c->n_repeats--; return true; }
  return now - c->last_sent >= (active(c, now) ? HEARTBEAT_ACTIVE_US : HEARTBEAT_IDLE_US);
}

size_t cwl2_packet(cwl2_t *c, uint64_t now, uint8_t *out) {
  // Forget edges older than the repeat window, but keep the newest one while the key is down.
  while (c->n_edges && now - c->edges[0].t > REPEAT_US && !(c->n_edges == 1 && c->key_down)) drop_oldest(c);
  int n = c->n_edges < MAX_EDGES ? c->n_edges : MAX_EDGES, from = c->n_edges - n;
  uint8_t *p = out;
  memcpy(p, "CWL2", 4); p += 4;
  *p++ = 1; *p++ = active(c, now) ? 1 : 0; *p++ = c->wpm; *p++ = (uint8_t)n;
  put(&p, c->session, 4); put(&p, ++c->seq, 4); put(&p, now, 8); put(&p, c->nonce, 8);
  put(&p, n ? c->edges[from].index : c->next_index, 4); put(&p, c->delay_ms, 2); put(&p, 0, 2);
  for (int i = from; i < c->n_edges; i++) put(&p, (c->edges[i].t << 1) | (c->edges[i].down ? 1 : 0), 8);
  uint8_t mac[32];
  cwl2_hmac(c->key, c->key_len, out, (size_t)(p - out), mac);
  memcpy(p, mac, TAG_LEN); p += TAG_LEN;
  c->last_sent = now; c->has_sent = true;
  return (size_t)(p - out);
}

bool cwl2_on_status(cwl2_t *c, const uint8_t *data, size_t len, uint64_t now) {
  if (len != CWL2_STATUS_LEN) return false;
  uint8_t mac[32], diff = 0;
  cwl2_hmac(c->key, c->key_len, data, len - TAG_LEN, mac);
  for (int i = 0; i < TAG_LEN; i++) diff |= mac[i] ^ data[len - TAG_LEN + i];   // compares in constant time
  if (diff || memcmp(data, "CWL2", 4) || data[4] != 2) return false;
  const uint8_t *p = data + 5;
  cwl2_status_t s;
  s.flags = *p++; s.mode = *p++; s.receiver = *p++;
  s.session = (uint32_t)get(&p, 4); s.seq = (uint32_t)get(&p, 4); s.echo_t = get(&p, 8); s.echo_age = (uint32_t)get(&p, 4);
  s.nonce = get(&p, 8); s.freq_hz = (uint32_t)get(&p, 4); s.delay_ms = (uint16_t)get(&p, 2);
  s.late = (uint16_t)get(&p, 2); s.lost = (uint16_t)get(&p, 2); s.rx_pkts = (uint16_t)get(&p, 2);
  c->nonce = s.nonce; c->status = s; c->has_status = true;
  if (s.session == c->session && s.echo_t) c->rtt_us = (int64_t)(now - s.echo_t - s.echo_age);
  return true;
}

bool cwl2_linked(const cwl2_t *c) {
  return c->has_status && c->status.session == c->session && !(c->status.flags & (CWL2_ST_MUTED | CWL2_ST_NONCE_BAD));
}
