#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

struct rng {
  uint64_t raw;
};

struct rng rng_new(void) {
  return (struct rng) {.raw = (uint64_t)time(NULL) ^ 0xdeadbeefcafef00dull};
}

uint32_t rng_next(struct rng *r) {
  assert(r);
  r->raw = r->raw * 0x3243f6a8885a308d + 1;
  return (uint32_t)(r->raw >> 32);
}

// torus cell (x,y) flattened to y*width+x, biased +1 to avoid raw==0 sentinel
struct key {
  uint64_t raw;
};

struct key key_new(uint32_t x, uint32_t y, uint32_t w) {
  return (struct key) {.raw = (uint64_t)y * w + x + 1u};
}

uint32_t key_x(struct key k, uint32_t w) {
  return (uint32_t)((k.raw - 1u) % w);
}

uint32_t key_y(struct key k, uint32_t w) {
  return (uint32_t)((k.raw - 1u) / w);
}

uint64_t key_hash(struct key k) {
  uint64_t h = k.raw;
  h ^= h >> 33;
  // murmur3 fmix64, adjacent keys scatter
  h *= 0xff51afd7ed558ccdull;
  h ^= h >> 33;
  return h;
}

bool key_empty(struct key k) {
  return k.raw == 0;
}

bool key_eq(struct key a, struct key b) {
  return a.raw == b.raw;
}

#define ALIVE_MASK (1u << 4)
#define COUNT_MASK (ALIVE_MASK - 1u)

struct slot {
  struct key key;
  uint32_t state;
};

struct table {
  size_t len;
  // indices of len written slots
  size_t *dirty;
  struct slot *slots;
  // number of hash index bits
  uint32_t exp;
};

void table_init(struct table *t, uint32_t exp) {
  assert(t);
  assert(exp >= 1 && exp <= 31); // 32 - exp must be a valid shift
  t->exp = exp;
  t->slots = calloc((1u << t->exp), sizeof(struct slot));
  t->dirty = malloc((1u << t->exp) * sizeof(size_t));
  assert(t->slots && t->dirty);
  t->len = 0;
}

void table_free(struct table *t) {
  assert(t);
  free(t->slots);
  free(t->dirty);
}

void table_clear(struct table *t) {
  assert(t);
  // only zero the slots we actually touched -- O(live), not O(capacity)
  for (size_t i = 0; i < t->len; ++i) {
    t->slots[t->dirty[i]].key.raw = 0;
  }
  t->len = 0;
}

// mask-step-index probe, lifted straight from shakespoof / nullprogram
struct slot *table_slot(struct table *t, struct key key) {
  uint32_t hash = (uint32_t)key_hash(key);
  uint32_t mask = (uint32_t)((1u << t->exp) - 1);
  uint32_t step = (hash >> (32u - t->exp)) | 1u;
  for (uint32_t idx = hash;;) {
    idx = (idx + step) & mask;
    struct slot *s = &t->slots[(size_t)idx];
    if (key_empty(s->key) || key_eq(s->key, key)) {
      return s;
    }
  }
}

// mock rust entry api, creates key with default state if it doesnt exist...
// - requires state to have a default...
struct slot *table_intern(struct table *t, struct key key) {
  struct slot *s = table_slot(t, key);
  if (key_empty(s->key)) {
    assert(t->len < (1u << t->exp));
    *s = (struct slot) {.key = key, .state = 0};
    t->dirty[t->len] = (size_t)(s - t->slots);
    t->len += 1;
  }
  return s;
}

struct world {
  size_t width;
  size_t height;
  // live cells this generation
  struct table cur;
  // scratch, neighbour counts + alive flags
  struct table acc;
};

// /cf every cell of the grid live or dead
static void fill_random(struct world *w, struct rng *rng) {
  assert(w);
  assert(rng);
  uint32_t width = (uint32_t)w->width;
  uint32_t height = (uint32_t)w->height;
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      if (rng_next(rng) & 1u) {
        table_intern(&w->cur, key_new(x, y, width));
      }
    }
  }
}

static uint32_t ceil_exp(size_t n) {
  uint32_t e = 1;
  while (((size_t)1 << e) < n) {
    e += 1;
  }
  // smallest exp with (1<<e) >=n
  return e;
}

void world_init(struct world *w, struct rng *rng, size_t width, size_t height) {
  assert(w);
  // non-empty, and <=UINT32_MAX>>1 so coords fit uint32_t (max 2*width); we can
  // add +width for toroidal arithmetic when calculating neighbour deltas...
  assert(width > 0 && width <= (size_t)(UINT32_MAX >> 1));
  assert(height > 0 && height <= (size_t)(UINT32_MAX >> 1));
  w->width = width;
  w->height = height;
  // both tables hold at most W*H distinct grid cells; size them to area plus
  // half-again headroom (load <= ~2/3) so the table stays cache-resident
  // instead of scattering across a fixed 2^20.
  size_t area = width * height;
  uint32_t exp = ceil_exp(area + area / 2);
  table_init(&w->cur, exp);
  table_init(&w->acc, exp);
  fill_random(w, rng);
}

void world_free(struct world *w) {
  assert(w);
  table_free(&w->cur);
  table_free(&w->acc);
}

static const int32_t DX[8] = {-1, -1, -1, 0, 0, 1, 1, 1};
static const int32_t DY[8] = {-1, 0, 1, -1, 1, -1, 0, 1};

void world_step(struct world *w) {
  assert(w);
  uint32_t width = (uint32_t)w->width;
  uint32_t height = (uint32_t)w->height;
  table_clear(&w->acc);

  // scatter
  for (size_t i = 0; i < w->cur.len; ++i) {
    struct slot *s = &w->cur.slots[w->cur.dirty[i]];
    uint32_t x = key_x(s->key, width);
    uint32_t y = key_y(s->key, width);
    // mark cell as alive in accumulator
    table_intern(&w->acc, s->key)->state |= ALIVE_MASK;
    for (size_t n = 0; n < 8u; ++n) {
      uint32_t nx = (x + width + (uint32_t)DX[n]) % width;
      uint32_t ny = (y + height + (uint32_t)DY[n]) % height;
      // reindex neighbours in accumulator
      table_intern(&w->acc, key_new(nx, ny, width))->state += 1u;
    }
  }

  // gather
  // - walk accumulated dirty cells
  table_clear(&w->cur);
  for (size_t i = 0; i < w->acc.len; ++i) {
    struct slot *s = &w->acc.slots[w->acc.dirty[i]];
    uint32_t count = s->state & COUNT_MASK;
    bool alive = (s->state & ALIVE_MASK) != 0;
    // add back those that meet ruleset
    if (count == 3u || (count == 2u && alive)) {
      table_intern(&w->cur, s->key);
    }
  }
}

void world_dump(struct world *w, FILE *out, size_t gen) {
  assert(w);
  assert(out);
  // bring cursor to home, flickers otherwise
  fputs("\x1b[H", stdout);
  printf("generation %zu (%zu live)\x1b[K\n", gen, w->cur.len);
  uint32_t width = (uint32_t)w->width;
  uint32_t height = (uint32_t)w->height;
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      bool live = !key_empty(table_slot(&w->cur, key_new(x, y, width))->key);
      fputc(' ', out);
      fputc(live ? 'X' : '.', out);
      fputc(' ', out);
    }
    fputc('\n', out);
  }
  fputc('\n', out);
  // flush remaining bytes
  fflush(out);
}

int main(void) {
  struct rng rng = rng_new();

  struct world w = {0};
  world_init(&w, &rng, 50, 50);

  // full buffer 1<<16 bytes
  setvbuf(stdout, NULL, _IOFBF, (size_t)1 << 16);

  // clear screen at start; dims are fixed
  fputs("\x1b[2J", stdout);
  for (size_t gen = 0;; ++gen) {
    world_dump(&w, stdout, gen);
    world_step(&w);
  }
  world_free(&w);

  return 0;
}
