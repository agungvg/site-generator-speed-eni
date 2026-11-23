// engine_run.c — full native engine for the make.fun site-generator entry.
// Included from engine.c (after the markdown kernel). Byte-exact port of
// run() from solve.js: JSON in, whole build in C, answers JSON out.
// Strings are UTF-8 bytes; lone surrogates are kept as CESU-8 (ED A0..BF)
// so the JSON serializer can re-emit them as \uXXXX, exactly like JS.
#include <time.h>
#include "nfkd_table.h"

// ---- chunked arena (pointers stay valid across growth) ----
typedef struct ABlock { struct ABlock *next; size_t used, cap; char data[]; } ABlock;
typedef struct { ABlock *head, *cur; } Arena;
static bool arena_init(Arena *a) {
  a->head = a->cur = NULL;
  return true;
}
static void *arena_alloc(Arena *a, size_t n) {
  n = (n + 7) & ~(size_t)7;
  if (!a->cur || a->cur->used + n > a->cur->cap) {
    size_t cap = n > (1u << 20) ? n : (1u << 20);
    ABlock *b = (ABlock *)malloc(sizeof(ABlock) + cap);
    if (!b) return NULL;
    b->next = NULL; b->used = 0; b->cap = cap;
    if (a->cur) a->cur->next = b; else a->head = b;
    a->cur = b;
  }
  void *p = a->cur->data + a->cur->used;
  a->cur->used += n;
  return p;
}
static char *arena_strndup(Arena *a, const char *s, size_t n) {
  char *p = (char *)arena_alloc(a, n + 1);
  if (!p) return NULL;
  memcpy(p, s, n); p[n] = 0;
  return p;
}
static void arena_reset(Arena *a) {
  for (ABlock *b = a->head; b; b = b->next) b->used = 0;
  a->cur = a->head;
}
static void arena_free(Arena *a) {
  ABlock *b = a->head;
  while (b) { ABlock *n = b->next; free(b); b = n; }
  a->head = a->cur = NULL;
}

// ---- UTF-8 helpers ----
typedef struct { char *s; size_t n; } Str;
// Decode one code point; invalid bytes consume 1 byte and yield 0xFFFD.
// CESU-8 surrogate halves (ED A0..ED BF) decode to 0xD800..0xDFFF.
static uint32_t utf8_decode(const char **pp, const char *end) {
  const unsigned char *p = (const unsigned char *)*pp;
  if (p >= (const unsigned char *)end) return 0xFFFFFFFFu;
  unsigned char c = *p;
  if (c < 0x80) { *pp = (const char *)(p + 1); return c; }
  if ((c & 0xE0) == 0xC0 && p + 1 < (const unsigned char *)end && ((p[1] & 0xC0) == 0x80)) {
    uint32_t cp = ((c & 0x1F) << 6) | (p[1] & 0x3F);
    if (cp >= 0x80) { *pp = (const char *)(p + 2); return cp; }
  } else if ((c & 0xF0) == 0xE0 && p + 2 < (const unsigned char *)end && ((p[1] & 0xC0) == 0x80) && ((p[2] & 0xC0) == 0x80)) {
    uint32_t cp = ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    if (cp >= 0x800 && !(cp >= 0xD800 && cp <= 0xDFFF)) { *pp = (const char *)(p + 3); return cp; }
    if (cp >= 0xD800 && cp <= 0xDFFF) { *pp = (const char *)(p + 3); return cp; } // CESU-8
    if (cp >= 0x800) { *pp = (const char *)(p + 3); return cp; }
  } else if ((c & 0xF8) == 0xF0 && p + 3 < (const unsigned char *)end && ((p[1] & 0xC0) == 0x80) && ((p[2] & 0xC0) == 0x80) && ((p[3] & 0xC0) == 0x80)) {
    uint32_t cp = ((c & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
    if (cp >= 0x10000 && cp <= 0x10FFFF) { *pp = (const char *)(p + 4); return cp; }
  }
  *pp = (const char *)(p + 1);
  return 0xFFFDu;
}
// Encode cp to UTF-8 (surrogates as CESU-8). Returns bytes written.
static int utf8_encode(uint32_t cp, char out[4]) {
  if (cp < 0x80) { out[0] = (char)cp; return 1; }
  if (cp < 0x800) { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
  if (cp < 0x10000) { out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
  out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F)); return 4;
}
// JS whitespace (String trim / \s) test on a code point.
static bool is_js_ws_cp(uint32_t cp) {
  return cp == 0x09 || cp == 0x0A || cp == 0x0B || cp == 0x0C || cp == 0x0D || cp == 0x20 ||
         cp == 0xA0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 ||
         cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x3000 || cp == 0xFEFF;
}
// Next UTF-16 code unit from a UTF-8 cursor (for JS string < ordering).
typedef struct { const char *p, *end; uint32_t pend; bool has_pend; } U16Cur;
static uint32_t u16_next(U16Cur *c) {
  if (c->has_pend) { c->has_pend = false; return c->pend; }
  if (c->p >= c->end) return 0xFFFFFFFFu;
  uint32_t cp = utf8_decode(&c->p, c->end);
  if (cp == 0xFFFFFFFFu) return 0xFFFFFFFFu;
  if (cp >= 0x10000) {
    uint32_t v = cp - 0x10000;
    c->pend = 0xDC00 + (v & 0x3FF);
    c->has_pend = true;
    return 0xD800 + (v >> 10);
  }
  return cp;
}
// JS string < comparison (UTF-16 code units). Returns -1/0/1.
static int cmp_utf16(const char *a, size_t an, const char *b, size_t bn) {
  U16Cur ca = { a, a + an, 0, false }, cb = { b, b + bn, 0, false };
  for (;;) {
    uint32_t x = u16_next(&ca), y = u16_next(&cb);
    if (x == 0xFFFFFFFFu && y == 0xFFFFFFFFu) return 0;
    if (x == 0xFFFFFFFFu) return -1;
    if (y == 0xFFFFFFFFu) return 1;
    if (x != y) return x < y ? -1 : 1;
  }
}
static int cmp_str(Str a, Str b) { return cmp_utf16(a.s, a.n, b.s, b.n); }

// ---- values ----
typedef enum { V_NULL, V_BOOL, V_NUM, V_STR, V_ARR, V_OBJ } VType;
typedef struct Val Val;
struct Val {
  VType t;
  bool b;
  double d;
  Str str;                       // V_STR
  Val **items; size_t nitems, capitems; // V_ARR
  Str *keys; Val **vals; size_t nobj, capobj; // V_OBJ
};
static Val *val_new(Arena *a, VType t) {
  Val *v = (Val *)arena_alloc(a, sizeof(Val));
  if (!v) return NULL;
  memset(v, 0, sizeof *v);
  v->t = t;
  return v;
}
static Val *val_str(Arena *a, const char *s, size_t n) {
  Val *v = val_new(a, V_STR);
  if (!v) return NULL;
  char *p = arena_strndup(a, s, n);
  if (!p) return NULL;
  v->str.s = p; v->str.n = n;
  return v;
}
static Val *val_cstr(Arena *a, const char *s) { return val_str(a, s, strlen(s)); }
static Val *val_num(Arena *a, double d) {
  Val *v = val_new(a, V_NUM);
  if (!v) return NULL;
  v->d = d;
  return v;
}
static Val *val_bool(Arena *a, bool b) {
  Val *v = val_new(a, V_BOOL);
  if (!v) return NULL;
  v->b = b;
  return v;
}
static Val *obj_get(Val *o, const char *key, size_t klen) {
  if (!o || o->t != V_OBJ) return NULL;
  for (size_t i = 0; i < o->nobj; i++)
    if (o->keys[i].n == klen && memcmp(o->keys[i].s, key, klen) == 0) return o->vals[i];
  return NULL;
}
static Val *obj_gets(Val *o, const char *key) { return obj_get(o, key, strlen(key)); }
static bool obj_set(Arena *a, Val *o, const char *key, size_t klen, Val *v) {
  if (!o || o->t != V_OBJ) return false;
  for (size_t i = 0; i < o->nobj; i++)
    if (o->keys[i].n == klen && memcmp(o->keys[i].s, key, klen) == 0) { o->vals[i] = v; return true; }
  if (o->nobj == o->capobj) {
    size_t nc = o->capobj ? o->capobj * 2 : 8;
    Str *nk = (Str *)arena_alloc(a, nc * sizeof(Str));
    Val **nv = (Val **)arena_alloc(a, nc * sizeof(Val *));
    if (!nk || !nv) return false;
    memcpy(nk, o->keys, o->nobj * sizeof(Str));
    memcpy(nv, o->vals, o->nobj * sizeof(Val *));
    o->keys = nk; o->vals = nv; o->capobj = nc;
  }
  char *kp = arena_strndup(a, key, klen);
  if (!kp) return false;
  o->keys[o->nobj].s = kp; o->keys[o->nobj].n = klen;
  o->vals[o->nobj] = v;
  o->nobj++;
  return true;
}
static bool obj_sets(Arena *a, Val *o, const char *key, Val *v) { return obj_set(a, o, key, strlen(key), v); }
static bool obj_has(Val *o, const char *key, size_t klen) { return obj_get(o, key, klen) != NULL; }
static bool obj_del(Val *o, const char *key, size_t klen) {
  if (!o || o->t != V_OBJ) return false;
  for (size_t i = 0; i < o->nobj; i++) {
    if (o->keys[i].n == klen && memcmp(o->keys[i].s, key, klen) == 0) {
      memmove(&o->keys[i], &o->keys[i + 1], (o->nobj - i - 1) * sizeof(Str));
      memmove(&o->vals[i], &o->vals[i + 1], (o->nobj - i - 1) * sizeof(Val *));
      o->nobj--;
      return true;
    }
  }
  return false;
}
static bool arr_push(Arena *a, Val *arr, Val *v) {
  if (!arr || arr->t != V_ARR) return false;
  if (arr->nitems == arr->capitems) {
    size_t nc = arr->capitems ? arr->capitems * 2 : 8;
    Val **ni = (Val **)arena_alloc(a, nc * sizeof(Val *));
    if (!ni) return false;
    memcpy(ni, arr->items, arr->nitems * sizeof(Val *));
    arr->items = ni; arr->capitems = nc;
  }
  arr->items[arr->nitems++] = v;
  return true;
}

// JS Number -> String (used by textOf). Integers print exactly; other
// numbers use the shortest round-trip decimal, JS-style exponent rules.
static void js_num_str(double d, char *buf, size_t cap) {
  if (d != d) { snprintf(buf, cap, "NaN"); return; }
  if (d == 1.0 / 0.0) { snprintf(buf, cap, "Infinity"); return; }
  if (d == -1.0 / 0.0) { snprintf(buf, cap, "-Infinity"); return; }
  if (d == 0) { snprintf(buf, cap, "0"); return; }
  double ad = d < 0 ? -d : d;
  if (ad < 1e21 && d == (double)(long long)d && ad < 9.007199254740992e15) {
    snprintf(buf, cap, "%lld", (long long)d);
    return;
  }
  // shortest round-trip via %.17g-style search, then JS exponent formatting.
  char tmp[64];
  for (int p = 1; p <= 17; p++) {
    snprintf(tmp, sizeof tmp, "%.*g", p, d);
    if (strtod(tmp, NULL) == d) break;
  }
  // %g already uses e-notation for very large/small; JS wants no leading
  // zero in the exponent and always a sign. Normalize "e-07" -> "e-7".
  char *e = strchr(tmp, 'e');
  if (e) {
    // JS switches to exponential when exp < -6 or exp >= 21; %g uses
    // exp < -4 or exp >= p. For our data (small ints) this never fires;
    // keep %g's choice and just fix the exponent format.
    char *s = e + 1;
    bool neg = false;
    if (*s == '+' || *s == '-') { neg = (*s == '-'); s++; }
    while (*s == '0') s++;
    snprintf(buf, cap, "%.*s%c%c%s", (int)(e - tmp), tmp, 'e', neg ? '-' : '+', s);
    return;
  }
  snprintf(buf, cap, "%s", tmp);
}

// ---- small open-addressing string map (keys point into an arena) ----
typedef struct { const char *k; size_t klen; void *v; bool used; } HEnt;
typedef struct { HEnt *ents; size_t cap, n; } HMap;
static uint64_t fnv1a(const char *s, size_t n) {
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 1099511628211ULL; }
  return h;
}
static bool hmap_init(HMap *m, size_t cap) {
  m->cap = cap; m->n = 0;
  m->ents = (HEnt *)calloc(cap, sizeof(HEnt));
  return m->ents != NULL;
}
static void hmap_free(HMap *m) { free(m->ents); m->ents = NULL; m->cap = m->n = 0; }
static void *hmap_get(HMap *m, const char *k, size_t klen) {
  if (!m->ents) return NULL;
  size_t i = (size_t)fnv1a(k, klen) & (m->cap - 1);
  while (m->ents[i].used) {
    if (m->ents[i].klen == klen && memcmp(m->ents[i].k, k, klen) == 0) return m->ents[i].v;
    i = (i + 1) & (m->cap - 1);
  }
  return NULL;
}
static bool hmap_put(HMap *m, const char *k, size_t klen, void *v) {
  if (!m->ents) return false;
  if ((m->n + 1) * 10 > m->cap * 7) { // grow at 70%
    size_t nc = m->cap * 2;
    HEnt *ne = (HEnt *)calloc(nc, sizeof(HEnt));
    if (!ne) return false;
    for (size_t i = 0; i < m->cap; i++) {
      if (!m->ents[i].used) continue;
      size_t j = (size_t)fnv1a(m->ents[i].k, m->ents[i].klen) & (nc - 1);
      while (ne[j].used) j = (j + 1) & (nc - 1);
      ne[j] = m->ents[i];
    }
    free(m->ents);
    m->ents = ne; m->cap = nc;
  }
  size_t i = (size_t)fnv1a(k, klen) & (m->cap - 1);
  while (m->ents[i].used) {
    if (m->ents[i].klen == klen && memcmp(m->ents[i].k, k, klen) == 0) { m->ents[i].v = v; return true; }
    i = (i + 1) & (m->cap - 1);
  }
  m->ents[i].used = true; m->ents[i].k = k; m->ents[i].klen = klen; m->ents[i].v = v;
  m->n++;
  return true;
}

// ---- JSON parser (input file -> Val tree) ----
typedef struct { const char *p, *end; Arena *a; bool err; } JParser;
static void jp_ws(JParser *jp) {
  while (jp->p < jp->end && (*jp->p == ' ' || *jp->p == '\t' || *jp->p == '\n' || *jp->p == '\r')) jp->p++;
}
static Val *jp_value(JParser *jp);
static bool jp_hex4(JParser *jp, uint32_t *out) {
  if (jp->end - jp->p < 4) return false;
  uint32_t v = 0;
  for (int i = 0; i < 4; i++) {
    char c = jp->p[i];
    v <<= 4;
    if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
    else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
    else return false;
  }
  jp->p += 4;
  *out = v;
  return true;
}
// Parse a JSON string starting at the opening quote. Returns arena Str.
static bool jp_string(JParser *jp, Str *out) {
  // jp->p points at '"'.
  jp->p++;
  Buf b = {0};
  bool ok = true, closed = false;
  while (jp->p < jp->end && ok) {
    unsigned char c = (unsigned char)*jp->p;
    if (c == '"') { jp->p++; closed = true; break; }
    if (c == '\\') {
      jp->p++;
      if (jp->p >= jp->end) { ok = false; break; }
      char e = *jp->p++;
      char tmp[4]; int tn = 0;
      switch (e) {
        case '"': tmp[0] = '"'; tn = 1; break;
        case '\\': tmp[0] = '\\'; tn = 1; break;
        case '/': tmp[0] = '/'; tn = 1; break;
        case 'b': tmp[0] = '\b'; tn = 1; break;
        case 'f': tmp[0] = '\f'; tn = 1; break;
        case 'n': tmp[0] = '\n'; tn = 1; break;
        case 'r': tmp[0] = '\r'; tn = 1; break;
        case 't': tmp[0] = '\t'; tn = 1; break;
        case 'u': {
          uint32_t cp;
          if (!jp_hex4(jp, &cp)) { ok = false; break; }
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            // high surrogate: look for a following \uDC00-\uDFFF
            if (jp->end - jp->p >= 6 && jp->p[0] == '\\' && jp->p[1] == 'u') {
              const char *save = jp->p;
              jp->p += 2;
              uint32_t lo;
              if (jp_hex4(jp, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
              } else { jp->p = save; }
            }
          }
          tn = utf8_encode(cp, tmp);
          break;
        }
        default: ok = false; break;
      }
      if (ok && tn > 0 && !buf_putn(&b, tmp, (size_t)tn)) ok = false;
      continue;
    }
    if (c < 0x20) { ok = false; break; }
    if (!buf_putc(&b, (char)c)) { ok = false; break; }
    jp->p++;
  }
  if (!ok || !closed) { free(b.p); jp->err = true; return false; }
  char *s = arena_strndup(jp->a, b.p ? b.p : "", b.len);
  free(b.p);
  if (!s) { jp->err = true; return false; }
  out->s = s; out->n = b.len;
  return true;
}
static Val *jp_value(JParser *jp) {
  jp_ws(jp);
  if (jp->p >= jp->end || jp->err) { jp->err = true; return NULL; }
  char c = *jp->p;
  if (c == '{') {
    jp->p++;
    Val *o = val_new(jp->a, V_OBJ);
    if (!o) { jp->err = true; return NULL; }
    jp_ws(jp);
    if (jp->p < jp->end && *jp->p == '}') { jp->p++; return o; }
    for (;;) {
      jp_ws(jp);
      if (jp->p >= jp->end || *jp->p != '"') { jp->err = true; return NULL; }
      Str key;
      if (!jp_string(jp, &key)) return NULL;
      jp_ws(jp);
      if (jp->p >= jp->end || *jp->p != ':') { jp->err = true; return NULL; }
      jp->p++;
      Val *v = jp_value(jp);
      if (!v) return NULL;
      if (!obj_set(jp->a, o, key.s, key.n, v)) { jp->err = true; return NULL; }
      jp_ws(jp);
      if (jp->p < jp->end && *jp->p == ',') { jp->p++; continue; }
      if (jp->p < jp->end && *jp->p == '}') { jp->p++; return o; }
      jp->err = true; return NULL;
    }
  }
  if (c == '[') {
    jp->p++;
    Val *arr = val_new(jp->a, V_ARR);
    if (!arr) { jp->err = true; return NULL; }
    jp_ws(jp);
    if (jp->p < jp->end && *jp->p == ']') { jp->p++; return arr; }
    for (;;) {
      Val *v = jp_value(jp);
      if (!v) return NULL;
      if (!arr_push(jp->a, arr, v)) { jp->err = true; return NULL; }
      jp_ws(jp);
      if (jp->p < jp->end && *jp->p == ',') { jp->p++; continue; }
      if (jp->p < jp->end && *jp->p == ']') { jp->p++; return arr; }
      jp->err = true; return NULL;
    }
  }
  if (c == '"') {
    Str s;
    if (!jp_string(jp, &s)) return NULL;
    return val_str(jp->a, s.s, s.n);
  }
  if (c == 't') {
    if (jp->end - jp->p >= 4 && memcmp(jp->p, "true", 4) == 0) { jp->p += 4; return val_bool(jp->a, true); }
    jp->err = true; return NULL;
  }
  if (c == 'f') {
    if (jp->end - jp->p >= 5 && memcmp(jp->p, "false", 5) == 0) { jp->p += 5; return val_bool(jp->a, false); }
    jp->err = true; return NULL;
  }
  if (c == 'n') {
    if (jp->end - jp->p >= 4 && memcmp(jp->p, "null", 4) == 0) { jp->p += 4; return val_new(jp->a, V_NULL); }
    jp->err = true; return NULL;
  }
  // number: -? digits (. digits)? ([eE][+-]? digits)?
  {
    const char *st = jp->p;
    if (jp->p < jp->end && *jp->p == '-') jp->p++;
    if (jp->p >= jp->end || *jp->p < '0' || *jp->p > '9') { jp->err = true; return NULL; }
    while (jp->p < jp->end && *jp->p >= '0' && *jp->p <= '9') jp->p++;
    if (jp->p < jp->end && *jp->p == '.') {
      jp->p++;
      if (jp->p >= jp->end || *jp->p < '0' || *jp->p > '9') { jp->err = true; return NULL; }
      while (jp->p < jp->end && *jp->p >= '0' && *jp->p <= '9') jp->p++;
    }
    if (jp->p < jp->end && (*jp->p == 'e' || *jp->p == 'E')) {
      jp->p++;
      if (jp->p < jp->end && (*jp->p == '+' || *jp->p == '-')) jp->p++;
      if (jp->p >= jp->end || *jp->p < '0' || *jp->p > '9') { jp->err = true; return NULL; }
      while (jp->p < jp->end && *jp->p >= '0' && *jp->p <= '9') jp->p++;
    }
    char numbuf[64];
    size_t nl = (size_t)(jp->p - st);
    if (nl >= sizeof numbuf) { jp->err = true; return NULL; }
    memcpy(numbuf, st, nl); numbuf[nl] = 0;
    return val_num(jp->a, strtod(numbuf, NULL));
  }
}

// ---- JSON serializer (JSON.stringify semantics) ----
static bool json_write_str(Buf *b, const char *s, size_t n) {
  // Fast path: copy runs of bytes that JSON.stringify leaves raw; only
  // '"', '\\', control chars < 0x20, and CESU-8 lone surrogates (ED A0..BF)
  // need escapes. Every other byte (incl. multi-byte UTF-8) passes through.
  if (!buf_putc(b, '"')) return false;
  const unsigned char *p = (const unsigned char *)s;
  const unsigned char *end = p + n;
  const unsigned char *run = p;
  while (p < end) {
    unsigned char c = *p;
    bool is_surrogate = (c == 0xED && p + 2 < end && (p[1] & 0xE0) == 0xA0 && (p[2] & 0xC0) == 0x80);
    if (c == '"' || c == '\\' || c < 0x20 || is_surrogate) {
      if (p > run && !buf_putn(b, (const char *)run, (size_t)(p - run))) return false;
      if (c == '"') { if (!buf_puts(b, "\\\"")) return false; }
      else if (c == '\\') { if (!buf_puts(b, "\\\\")) return false; }
      else if (c == 0x08) { if (!buf_puts(b, "\\b")) return false; }
      else if (c == 0x0C) { if (!buf_puts(b, "\\f")) return false; }
      else if (c == 0x0A) { if (!buf_puts(b, "\\n")) return false; }
      else if (c == 0x0D) { if (!buf_puts(b, "\\r")) return false; }
      else if (c == 0x09) { if (!buf_puts(b, "\\t")) return false; }
      else if (c < 0x20) {
        char esc[8];
        snprintf(esc, sizeof esc, "\\u%04x", c);
        if (!buf_puts(b, esc)) return false;
      } else {
        // CESU-8 3-byte sequence ED A0..BF: cp = 0xD000 + ((b1&0x3F)<<6) + (b2&0x3F)
        uint32_t cp = 0xD000u + (uint32_t)((p[1] & 0x3F) << 6) + (uint32_t)(p[2] & 0x3F);
        char esc[8];
        snprintf(esc, sizeof esc, "\\u%04x", cp);
        if (!buf_puts(b, esc)) return false;
        p += 2;
      }
      p++;
      run = p;
    } else {
      p++;
    }
  }
  if (p > run && !buf_putn(b, (const char *)run, (size_t)(p - run))) return false;
  if (!buf_putc(b, '"')) return false;
  return true;
}
static bool json_write_val(Buf *b, Val *v) {
  switch (v->t) {
    case V_NULL: return buf_puts(b, "null");
    case V_BOOL: return buf_puts(b, v->b ? "true" : "false");
    case V_NUM: { char nb[64]; js_num_str(v->d, nb, sizeof nb); return buf_puts(b, nb); }
    case V_STR: return json_write_str(b, v->str.s, v->str.n);
    case V_ARR: {
      if (!buf_putc(b, '[')) return false;
      for (size_t i = 0; i < v->nitems; i++) {
        if (i && !buf_putc(b, ',')) return false;
        if (!json_write_val(b, v->items[i])) return false;
      }
      return buf_putc(b, ']');
    }
    case V_OBJ: {
      if (!buf_putc(b, '{')) return false;
      for (size_t i = 0; i < v->nobj; i++) {
        if (i && !buf_putc(b, ',')) return false;
        if (!json_write_str(b, v->keys[i].s, v->keys[i].n)) return false;
        if (!buf_putc(b, ':')) return false;
        if (!json_write_val(b, v->vals[i])) return false;
      }
      return buf_putc(b, '}');
    }
  }
  return false;
}

// ---- slugify (NFKD + mark strip + ASCII filter, cached per case) ----
static bool is_mark_cp(uint32_t cp) {
  size_t lo = 0, hi = NFKD_MARKS_N;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (cp < NFKD_MARKS[mid].lo) hi = mid;
    else if (cp > NFKD_MARKS[mid].hi) lo = mid + 1;
    else return true;
  }
  return false;
}
static const uint32_t *nfkd_lookup(uint32_t cp, uint32_t *len) {
  size_t lo = 0, hi = NFKD_DECOMP_N;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (cp < NFKD_DECOMP[mid].cp) hi = mid;
    else if (cp > NFKD_DECOMP[mid].cp) lo = mid + 1;
    else { *len = NFKD_DECOMP[mid].len; return &NFKD_POOL[NFKD_DECOMP[mid].off]; }
  }
  return NULL;
}
static bool cp_push(uint32_t **buf, size_t *n, size_t *cap, uint32_t cp) {
  if (*n == *cap) {
    size_t nc = *cap ? *cap * 2 : 64;
    uint32_t *nb = (uint32_t *)realloc(*buf, nc * sizeof(uint32_t));
    if (!nb) return false;
    *buf = nb; *cap = nc;
  }
  (*buf)[(*n)++] = cp;
  return true;
}
static bool nfkd_expand_into(uint32_t cp, uint32_t **buf, size_t *n, size_t *cap) {
  uint32_t len = 0;
  const uint32_t *d = nfkd_lookup(cp, &len);
  if (!d) return cp_push(buf, n, cap, cp);
  for (uint32_t i = 0; i < len; i++)
    if (!nfkd_expand_into(d[i], buf, n, cap)) return false;
  return true;
}
// Returns an arena string (NUL-terminated). NULL on OOM.
static char *slugify_c(Arena *a, HMap *cache, const char *s, size_t n, size_t *out_len) {
  Str *hit = (Str *)hmap_get(cache, s, n);
  if (hit) { *out_len = hit->n; return hit->s; }
  uint32_t *cps = NULL; size_t ncps = 0, capcps = 0;
  const char *p = s, *end = s + n;
  bool ok = true;
  while (p < end && ok) {
    uint32_t cp = utf8_decode(&p, end);
    if (cp == 0xFFFFFFFFu) break;
    if (!nfkd_expand_into(cp, &cps, &ncps, &capcps)) ok = false;
  }
  Buf b = {0};
  if (ok) {
    bool gap = false;
    for (size_t i = 0; i < ncps && ok; i++) {
      uint32_t cp = cps[i];
      if (is_mark_cp(cp)) continue;
      char keep = 0;
      if (cp >= 'A' && cp <= 'Z') keep = (char)(cp + 32);
      else if ((cp >= 'a' && cp <= 'z') || (cp >= '0' && cp <= '9')) keep = (char)cp;
      if (keep == 0) { gap = b.len > 0; continue; }
      if (gap && !buf_putc(&b, '-')) { ok = false; break; }
      gap = false;
      if (!buf_putc(&b, keep)) { ok = false; break; }
    }
  }
  free(cps);
  if (!ok) { free(b.p); return NULL; }
  if (b.len > 64) b.len = 64;
  while (b.len > 0 && b.p[b.len - 1] == '-') b.len--;
  if (b.len == 0) {
    if (!buf_puts(&b, "untitled")) { free(b.p); return NULL; }
  }
  char *slug = arena_strndup(a, b.p ? b.p : "", b.len);
  free(b.p);
  if (!slug) return NULL;
  Str *ent = (Str *)arena_alloc(a, sizeof(Str));
  if (!ent) return NULL;
  ent->s = slug; ent->n = b.len;
  // The cache key must point at stable storage: the caller's string lives
  // in the input/case arena, which outlives the case. Copy it for safety
  // when it points at a transient buffer (heading text ranges are stable
  // too, but be safe: copy the key into the arena).
  char *key = arena_strndup(a, s, n);
  if (!key) return NULL;
  if (!hmap_put(cache, key, n, ent)) return NULL;
  *out_len = ent->n;
  return slug;
}

// ---- front matter ----
static const char *FM_KEYS[] = { "title", "date", "tags", "draft", "layout", "slug", "summary" };
static bool fm_is_key(const char *k, size_t n) {
  for (size_t i = 0; i < sizeof(FM_KEYS) / sizeof(*FM_KEYS); i++)
    if (strlen(FM_KEYS[i]) == n && memcmp(FM_KEYS[i], k, n) == 0) return true;
  return false;
}
static int fm_days_in(int y, int m) {
  if (m == 2) return ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 29 : 28;
  return (m == 4 || m == 6 || m == 9 || m == 11) ? 30 : 31;
}
typedef struct { bool is_error; char *err; Val *data; Str body; } FMResult;
static bool fm_err(Arena *a, FMResult *r, const char *msg) {
  r->is_error = true;
  r->err = arena_strndup(a, msg, strlen(msg));
  return false;
}
static bool fm_errf(Arena *a, FMResult *r, const char *fmt, const char *arg) {
  char tmp[512];
  snprintf(tmp, sizeof tmp, fmt, arg);
  return fm_err(a, r, tmp);
}
// stringOf: returns false + sets r error, or fills *val.
static bool fm_string_of(Arena *a, const char *raw, size_t n, FMResult *r, Val **val) {
  if (n > 0 && raw[0] == '"') {
    if (n < 2 || raw[n - 1] != '"') return fm_err(a, r, "a quoted string is not closed");
    Buf b = {0};
    bool ok = true;
    int errkind = 0; // 1 = bad escape, 2 = stray quote
    for (size_t i = 1; i + 1 < n && ok; i++) {
      char c = raw[i];
      if (c == '\\') {
        char nx = raw[i + 1];
        if ((nx != '"' && nx != '\\') || i + 1 == n - 1) { ok = false; errkind = 1; break; }
        if (!buf_putc(&b, nx)) { ok = false; break; }
        i++;
      } else if (c == '"') { ok = false; errkind = 2; break; }
      else if (!buf_putc(&b, c)) { ok = false; break; }
    }
    if (!ok) {
      free(b.p);
      if (errkind == 2) return fm_err(a, r, "a quoted string holds a stray quote");
      if (errkind == 1) return fm_err(a, r, "a quoted string holds an escape other than \\\" or \\\\");
      return fm_err(a, r, "out of memory");
    }
    Val *v = val_str(a, b.p ? b.p : "", b.len);
    free(b.p);
    if (!v) return fm_err(a, r, "out of memory");
    *val = v;
    return true;
  }
  if (n > 0 && raw[0] == '[') return fm_err(a, r, "a bare string opens on [");
  Val *v = val_str(a, raw, n);
  if (!v) return fm_err(a, r, "out of memory");
  *val = v;
  return true;
}
static bool fm_is_tag_char(char c) { return c != '"' && c != '[' && c != ']'; }
// valueOf for one key. On error returns false with r->err set (message is
// the bare valueOf error, WITHOUT the `key "x": ` prefix; the caller adds
// it for title/summary).
static bool fm_value_of(Arena *a, const char *key, size_t klen, const char *raw, size_t n, FMResult *r, Val **val) {
  if ((klen == 5 && memcmp(key, "title", 5) == 0) || (klen == 7 && memcmp(key, "summary", 7) == 0)) {
    // stringOf errors get the `key "x": ` prefix in JS.
    FMResult sub = {0};
    if (!fm_string_of(a, raw, n, &sub, val)) {
      char tmp[512];
      snprintf(tmp, sizeof tmp, "key \"%.*s\": %s", (int)klen, key, sub.err ? sub.err : "error");
      return fm_err(a, r, tmp);
    }
    return true;
  }
  if (klen == 4 && memcmp(key, "date", 4) == 0) {
    bool ok = false;
    int y = 0, mo = 0, d = 0;
    if (n == 10 && raw[4] == '-' && raw[7] == '-') {
      bool dig = true;
      for (int i = 0; i < 10; i++) {
        if (i == 4 || i == 7) continue;
        if (raw[i] < '0' || raw[i] > '9') dig = false;
      }
      if (dig) {
        y = (raw[0] - '0') * 1000 + (raw[1] - '0') * 100 + (raw[2] - '0') * 10 + (raw[3] - '0');
        mo = (raw[5] - '0') * 10 + (raw[6] - '0');
        d = (raw[8] - '0') * 10 + (raw[9] - '0');
        if (y >= 1970 && y <= 2099 && mo >= 1 && mo <= 12 && d >= 1 && d <= fm_days_in(y, mo)) ok = true;
      }
    }
    if (!ok) return fm_err(a, r, "key \"date\": not a date from 1970-01-01 to 2099-12-31 (YYYY-MM-DD)");
    Val *v = val_str(a, raw, n);
    if (!v) return fm_err(a, r, "out of memory");
    *val = v;
    return true;
  }
  if (klen == 4 && memcmp(key, "tags", 4) == 0) {
    if (n < 2 || raw[0] != '[' || raw[n - 1] != ']') return fm_err(a, r, "key \"tags\": not a list [a, b]");
    // inner = raw[1..n-2]; trim JS-whitespace; empty -> [].
    const char *ip = raw + 1, *ie = raw + n - 1;
    while (ip < ie) { const char *q = ip; uint32_t cp = utf8_decode(&q, ie); if (!is_js_ws_cp(cp)) break; ip = q; }
    while (ie > ip) {
      const char *q = ie - 1;
      // step back one code point: find its start
      while (q > ip && ((unsigned char)*q & 0xC0) == 0x80) q--;
      const char *qq = q; uint32_t cp = utf8_decode(&qq, ie);
      if (!is_js_ws_cp(cp)) break;
      ie = q;
    }
    Val *arr = val_new(a, V_ARR);
    if (!arr) return fm_err(a, r, "out of memory");
    if (ip < ie) {
      // split on ','; trim each; validate TAG.
      const char *seg = ip;
      for (const char *p = ip; ; p++) {
        if (p == ie || *p == ',') {
          const char *s0 = seg, *s1 = p;
          while (s0 < s1) { const char *q = s0; uint32_t cp = utf8_decode(&q, s1); if (!is_js_ws_cp(cp)) break; s0 = q; }
          while (s1 > s0) {
            const char *q = s1 - 1;
            while (q > s0 && ((unsigned char)*q & 0xC0) == 0x80) q--;
            const char *qq = q; uint32_t cp = utf8_decode(&qq, s1);
            if (!is_js_ws_cp(cp)) break;
            s1 = q;
          }
          bool bad = (s1 == s0);
          if (!bad) for (const char *q = s0; q < s1; q++) if (!fm_is_tag_char(*q)) { bad = true; break; }
          if (bad) return fm_err(a, r, "key \"tags\": an empty tag, or one holding \" [ or ]");
          Val *tv = val_str(a, s0, (size_t)(s1 - s0));
          if (!tv || !arr_push(a, arr, tv)) return fm_err(a, r, "out of memory");
          seg = p + 1;
        }
        if (p == ie) break;
      }
    }
    *val = arr;
    return true;
  }
  if (klen == 5 && memcmp(key, "draft", 5) == 0) {
    if (n == 4 && memcmp(raw, "true", 4) == 0) { *val = val_bool(a, true); return *val != NULL; }
    if (n == 5 && memcmp(raw, "false", 5) == 0) { *val = val_bool(a, false); return *val != NULL; }
    return fm_err(a, r, "key \"draft\": true or false");
  }
  if (klen == 6 && memcmp(key, "layout", 6) == 0) {
    bool ok = n > 0;
    for (size_t i = 0; i < n && ok; i++) {
      char c = raw[i];
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) ok = false;
    }
    if (!ok) return fm_err(a, r, "key \"layout\": not a template name");
    Val *v = val_str(a, raw, n);
    if (!v) return fm_err(a, r, "out of memory");
    *val = v;
    return true;
  }
  if (klen == 4 && memcmp(key, "slug", 4) == 0) {
    bool ok = n > 0 && n <= 64;
    // /^[a-z0-9]+(?:-[a-z0-9]+)*$/
    if (ok) {
      bool need_alnum = true;
      for (size_t i = 0; i < n; i++) {
        char c = raw[i];
        if (c == '-') { if (need_alnum) { ok = false; break; } need_alnum = true; }
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) need_alnum = false;
        else { ok = false; break; }
      }
      if (need_alnum) ok = false;
    }
    if (!ok) return fm_err(a, r, "key \"slug\": not a slug");
    Val *v = val_str(a, raw, n);
    if (!v) return fm_err(a, r, "out of memory");
    *val = v;
    return true;
  }
  return fm_errf(a, r, "unknown key \"%.*s\"", key);
}
// Parse front matter. text is the normalized (\n) source. Fills r.
static bool parse_front_matter(Arena *a, const char *text, size_t n, FMResult *r) {
  memset(r, 0, sizeof *r);
  bool starts = (n >= 4 && memcmp(text, "---\n", 4) == 0);
  if (!starts && !(n == 3 && memcmp(text, "---", 3) == 0)) return fm_err(a, r, "no front matter");
  if (n == 3) return fm_err(a, r, "the front matter is not closed");
  // Find the closing line "---": it must be a whole line.
  long close = -1;
  size_t pos = 4;
  for (;;) {
    // indexOf('\n---', pos-1)
    long nl = -1;
    for (size_t i = (pos - 1); i + 4 <= n; i++) {
      if (text[i] == '\n' && memcmp(text + i + 1, "---", 3) == 0) { nl = (long)i; break; }
    }
    if (nl == -1) break;
    size_t after = (size_t)nl + 4;
    if (after == n || text[after] == '\n') { close = nl + 1; break; }
    pos = (size_t)nl + 4;
  }
  if (close == -1) return fm_err(a, r, "the front matter is not closed");
  // fmText = text[4 .. close-1); JS slice with end < start is ''.
  const char *fm = text + 4;
  size_t fmn = (close - 1 > 4) ? (size_t)(close - 1) - 4 : 0;
  Val *data = val_new(a, V_OBJ);
  if (!data) return fm_err(a, r, "out of memory");
  if (fmn > 0) {
    // split fmText on '\n'
    size_t ls = 0;
    for (size_t li = 0; ; li++) {
      size_t le = ls;
      while (le < fmn && fm[le] != '\n') le++;
      const char *line = fm + ls;
      size_t llen = le - ls;
      // blank or comment?
      bool blank = true;
      for (size_t i = 0; i < llen; i++) if (line[i] != ' ' && line[i] != '\t') { blank = false; break; }
      if (!blank && line[0] != '#') {
        // LINE: /^([a-z_]+):(?: (.*))?$/
        size_t k = 0;
        while (k < llen && ((line[k] >= 'a' && line[k] <= 'z') || line[k] == '_')) k++;
        if (k == 0 || k >= llen || line[k] != ':') {
          char tmp[64];
          snprintf(tmp, sizeof tmp, "line %zu is not \"key: value\"", li + 2);
          return fm_err(a, r, tmp);
        }
        const char *key = line;
        size_t klen = k;
        const char *raw = "";
        size_t rlen = 0;
        if (k + 1 < llen) {
          if (line[k + 1] != ' ') {
            char tmp[64];
            snprintf(tmp, sizeof tmp, "line %zu is not \"key: value\"", li + 2);
            return fm_err(a, r, tmp);
          }
          raw = line + k + 2;
          rlen = llen - (k + 2);
        } else if (k + 1 == llen) {
          raw = "";
          rlen = 0;
        }
        // raw = raw.replace(/[ \t]+$/, '')
        while (rlen > 0 && (raw[rlen - 1] == ' ' || raw[rlen - 1] == '\t')) rlen--;
        if (!fm_is_key(key, klen)) {
          char tmp[128];
          snprintf(tmp, sizeof tmp, "unknown key \"%.*s\"", (int)klen, key);
          return fm_err(a, r, tmp);
        }
        if (obj_has(data, key, klen)) {
          char tmp[128];
          snprintf(tmp, sizeof tmp, "key \"%.*s\" is given twice", (int)klen, key);
          return fm_err(a, r, tmp);
        }
        if (rlen == 0) {
          char tmp[128];
          snprintf(tmp, sizeof tmp, "key \"%.*s\" has no value", (int)klen, key);
          return fm_err(a, r, tmp);
        }
        Val *v = NULL;
        if (!fm_value_of(a, key, klen, raw, rlen, r, &v)) return false;
        if (!obj_set(a, data, key, klen, v)) return fm_err(a, r, "out of memory");
      }
      if (le >= fmn) break;
      ls = le + 1;
    }
  }
  if (!obj_has(data, "title", 5)) return fm_err(a, r, "no title");
  size_t body_start = (size_t)close + 4;
  r->data = data;
  if (body_start <= n) { r->body.s = (char *)text + body_start; r->body.n = n - body_start; }
  else { r->body.s = (char *)text + n; r->body.n = 0; }
  r->is_error = false;
  return true;
}

// wordsOf: count words (isWs per UTF-16 code unit).
static bool fm_is_ws_cp(uint32_t cp) {
  return cp <= 32 || cp == 160 || cp == 5760 || (cp >= 8192 && cp <= 8202) ||
         cp == 8232 || cp == 8233 || cp == 8239 || cp == 8287 || cp == 12288 || cp == 65279;
}
static long words_of(const char *s, size_t n) {
  long cnt = 0;
  bool in_word = false;
  const char *p = s, *end = s + n;
  while (p < end) {
    uint32_t cp = utf8_decode(&p, end);
    if (cp == 0xFFFFFFFFu) break;
    if (fm_is_ws_cp(cp)) in_word = false;
    else if (!in_word) { in_word = true; cnt++; }
  }
  return cnt;
}

static const char *MONTHS[] = { "January", "February", "March", "April", "May", "June", "July",
  "August", "September", "October", "November", "December" };
// "2026-11-09" -> "9 November 2026". date is a validated YYYY-MM-DD string.
static void long_date(const char *date, char *out, size_t cap) {
  int day = (date[8] - '0') * 10 + (date[9] - '0');
  int mo = (date[5] - '0') * 10 + (date[6] - '0');
  snprintf(out, cap, "%d %s %.4s", day, MONTHS[mo - 1], date);
}

// resolvePath(dir, rel): false when escaping above the root.
static bool resolve_path_c(const char *dir, size_t dirn, const char *rel, size_t reln, Arena *a, Str *out) {
  // segments of dir
  const char **segs = NULL;
  size_t nsegs = 0, capsegs = 0;
  // (use a small stack buffer via arena for simplicity: worst case segments
  // are bounded by path length)
  const char **tmp = (const char **)arena_alloc(a, (dirn + reln + 2) * sizeof(char *));
  size_t *lens = (size_t *)arena_alloc(a, (dirn + reln + 2) * sizeof(size_t));
  if (!tmp || !lens) return false;
  segs = tmp;
  if (dirn > 0) {
    size_t st = 0;
    for (size_t i = 0; i <= dirn; i++) {
      if (i == dirn || dir[i] == '/') {
        segs[nsegs] = dir + st; lens[nsegs] = i - st; nsegs++;
        st = i + 1;
      }
    }
  }
  // walk rel segments
  size_t st = 0;
  for (size_t i = 0; i <= reln; i++) {
    if (i == reln || rel[i] == '/') {
      size_t sl = i - st;
      const char *sg = rel + st;
      if (sl == 0 || (sl == 1 && sg[0] == '.')) {
        // skip
      } else if (sl == 2 && sg[0] == '.' && sg[1] == '.') {
        if (nsegs == 0) return false;
        nsegs--;
      } else {
        segs[nsegs] = sg; lens[nsegs] = sl; nsegs++;
      }
      st = i + 1;
    }
  }
  Buf b = {0};
  for (size_t i = 0; i < nsegs; i++) {
    if (i && !buf_putc(&b, '/')) { free(b.p); return false; }
    if (!buf_putn(&b, segs[i], lens[i])) { free(b.p); return false; }
  }
  char *s = arena_strndup(a, b.p ? b.p : "", b.len);
  free(b.p);
  if (!s) return false;
  out->s = s; out->n = b.len;
  return true;
}
static Str dir_of(const char *path, size_t n) {
  const char *slash = NULL;
  for (size_t i = 0; i < n; i++) if (path[i] == '/') slash = path + i;
  if (!slash) { Str r = { (char *)path + n, 0 }; return r; }
  Str r = { (char *)path, (size_t)(slash - path) };
  return r;
}

// ---- templates ----
typedef enum { N_ROOT, N_TEXT, N_VAR, N_RAW, N_IF, N_FOR, N_INCLUDE } NKind;
typedef struct TNode TNode;
struct TNode {
  NKind kind;
  Str text;                    // N_TEXT
  Str *path; size_t npath;     // N_VAR/N_RAW/N_IF/N_FOR
  bool not;                    // N_IF
  Str loop_name;               // N_FOR
  TNode **kids; size_t nkids, capkids;     // body / then
  TNode **kids2; size_t nkids2, capkids2;  // else (N_IF)
  Str inc_name;                // N_INCLUDE
};
static TNode *tnode_new(Arena *a, NKind kind) {
  TNode *n = (TNode *)arena_alloc(a, sizeof(TNode));
  if (!n) return NULL;
  memset(n, 0, sizeof *n);
  n->kind = kind;
  return n;
}
static bool tnode_kid(Arena *a, TNode *p, TNode *c, bool second) {
  TNode ***arr = second ? &p->kids2 : &p->kids;
  size_t *n = second ? &p->nkids2 : &p->nkids;
  size_t *cap = second ? &p->capkids2 : &p->capkids;
  if (*n == *cap) {
    size_t nc = *cap ? *cap * 2 : 8;
    TNode **na = (TNode **)arena_alloc(a, nc * sizeof(TNode *));
    if (!na) return false;
    memcpy(na, *arr, *n * sizeof(TNode *));
    *arr = na; *cap = nc;
  }
  (*arr)[(*n)++] = c;
  return true;
}
// JS String.trim on a byte range (Unicode whitespace).
static void js_trim(const char *s, size_t n, const char **a, size_t *an) {
  const char *p = s, *end = s + n;
  while (p < end) { const char *q = p; uint32_t cp = utf8_decode(&q, end); if (!is_js_ws_cp(cp)) break; p = q; }
  while (end > p) {
    const char *q = end - 1;
    while (q > p && ((unsigned char)*q & 0xC0) == 0x80) q--;
    const char *qq = q; uint32_t cp = utf8_decode(&qq, end);
    if (!is_js_ws_cp(cp)) break;
    end = q;
  }
  *a = p; *an = (size_t)(end - p);
}
// JSON.stringify of a short string, for TemplateError messages.
static char *json_quote(Arena *a, const char *s, size_t n) {
  Buf b = {0};
  if (!json_write_str(&b, s, n)) { free(b.p); return NULL; }
  char *r = arena_strndup(a, b.p ? b.p : "", b.len);
  free(b.p);
  return r;
}
static bool is_ident_start(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_'; }
static bool is_ident_char(char c) { return is_ident_start(c) || (c >= '0' && c <= '9'); }
// PATH_RE: ident(.ident)* — returns segments or NULL (sets *err).
static bool path_of(Arena *a, const char *s, size_t n, const char *tname, Str **out_path, size_t *out_n, char **err) {
  size_t i = 0;
  if (n == 0 || !is_ident_start(s[0])) goto bad;
  // validate + count segments
  {
    size_t nseg = 1;
    i = 0;
    while (i < n) {
      if (!is_ident_start(s[i])) goto bad;
      i++;
      while (i < n && is_ident_char(s[i])) i++;
      if (i == n) break;
      if (s[i] != '.') goto bad;
      i++;
      if (i >= n) goto bad;
      nseg++;
    }
    Str *segs = (Str *)arena_alloc(a, nseg * sizeof(Str));
    if (!segs) { *err = (char *)"out of memory"; return false; }
    size_t k = 0, st = 0;
    for (i = 0; i <= n; i++) {
      if (i == n || s[i] == '.') { segs[k].s = (char *)s + st; segs[k].n = i - st; k++; st = i + 1; }
    }
    *out_path = segs; *out_n = nseg;
    return true;
  }
bad: {
    char *q = json_quote(a, s, n);
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "template \"%s\": %s is not a path", tname, q ? q : "\"?\"");
    *err = arena_strndup(a, tmp, strlen(tmp));
    return false;
  }
}

typedef enum { TK_TEXT, TK_VAR, TK_RAW, TK_TAG } TKKind;
typedef struct { TKKind kind; Str text; Str *path; size_t npath; Str tag; } Token;
typedef struct { Token *toks; size_t n, cap; } TokList;
static bool tok_push(Arena *a, TokList *l, Token t) {
  if (l->n == l->cap) {
    size_t nc = l->cap ? l->cap * 2 : 32;
    Token *nt = (Token *)arena_alloc(a, nc * sizeof(Token));
    if (!nt) return false;
    memcpy(nt, l->toks, l->n * sizeof(Token));
    l->toks = nt; l->cap = nc;
  }
  l->toks[l->n++] = t;
  return true;
}
// tokenize(name, src): returns false + *err on TemplateError.
static bool tokenize(Arena *a, const char *tname, const char *src, size_t n, TokList *out, char **err) {
  memset(out, 0, sizeof *out);
  Buf text = {0}; // accumulated literal text
  size_t i = 0;
  bool ok = true;
  while (i < n && ok) {
    // open = indexOf('{', i)
    size_t open = n;
    for (size_t j = i; j < n; j++) if (src[j] == '{') { open = j; break; }
    if (open == n || open == n - 1) { if (!buf_putn(&text, src + i, n - i)) ok = false; break; }
    char next = src[open + 1];
    if (next != '{' && next != '%') {
      if (!buf_putn(&text, src + i, open + 1 - i)) ok = false;
      i = open + 1;
      continue;
    }
    if (!buf_putn(&text, src + i, open - i)) { ok = false; break; }
    if (next == '{') {
      bool raw = (open + 2 < n && src[open + 2] == '{');
      const char *closestr = raw ? "}}}" : "}}";
      size_t clen = raw ? 3 : 2;
      size_t base = open + clen;
      long close = -1;
      for (size_t j = base; j + clen <= n; j++) {
        if (memcmp(src + j, closestr, clen) == 0) { close = (long)j; break; }
      }
      if (close == -1) {
        char tmp[256];
        snprintf(tmp, sizeof tmp, "template \"%s\": a %s is not closed", tname, raw ? "{{{" : "{{");
        *err = arena_strndup(a, tmp, strlen(tmp));
        free(text.p);
        return false;
      }
      if (text.len > 0) {
        Token t = {0}; t.kind = TK_TEXT; t.text.s = arena_strndup(a, text.p, text.len); t.text.n = text.len;
        if (!t.text.s || !tok_push(a, out, t)) { ok = false; break; }
        text.len = 0;
      }
      const char *ps = src + base;
      size_t pn = (size_t)close - base;
      const char *ta; size_t tan;
      js_trim(ps, pn, &ta, &tan);
      Str *path = NULL; size_t npath = 0;
      if (!path_of(a, ta, tan, tname, &path, &npath, err)) { free(text.p); return false; }
      Token t = {0}; t.kind = raw ? TK_RAW : TK_VAR; t.path = path; t.npath = npath;
      if (!tok_push(a, out, t)) { ok = false; break; }
      i = (size_t)close + clen;
      continue;
    }
    // {% ... %}
    long close = -1;
    for (size_t j = open + 2; j + 2 <= n; j++) {
      if (src[j] == '%' && src[j + 1] == '}') { close = (long)j; break; }
    }
    if (close == -1) {
      char tmp[256];
      snprintf(tmp, sizeof tmp, "template \"%s\": a {%% is not closed", tname);
      *err = arena_strndup(a, tmp, strlen(tmp));
      free(text.p);
      return false;
    }
    size_t end = (size_t)close + 2;
    // lineStart = lastIndexOf('\n', open-1) + 1
    long line_start = 0;
    if (open > 0) {
      long ls = -1;
      for (long j = (long)open - 1; j >= 0; j--) if (src[j] == '\n') { ls = j; break; }
      line_start = ls + 1;
    }
    long nl = -1;
    for (size_t j = end; j < n; j++) if (src[j] == '\n') { nl = (long)j; break; }
    size_t line_end = nl == -1 ? n : (size_t)nl;
    bool before_ws = true;
    for (size_t j = (size_t)line_start; j < open; j++) if (src[j] != ' ' && src[j] != '\t') { before_ws = false; break; }
    bool after_ws = true;
    for (size_t j = end; j < line_end; j++) if (src[j] != ' ' && src[j] != '\t') { after_ws = false; break; }
    bool standalone = before_ws && after_ws;
    if (standalone) text.len -= (open - (size_t)line_start);
    if (text.len > 0) {
      Token t = {0}; t.kind = TK_TEXT; t.text.s = arena_strndup(a, text.p, text.len); t.text.n = text.len;
      if (!t.text.s || !tok_push(a, out, t)) { ok = false; break; }
      text.len = 0;
    }
    const char *ts = src + open + 2;
    size_t tn = (size_t)close - (open + 2);
    const char *ta; size_t tan;
    js_trim(ts, tn, &ta, &tan);
    Token t = {0}; t.kind = TK_TAG; t.tag.s = (char *)ta; t.tag.n = tan;
    if (!tok_push(a, out, t)) { ok = false; break; }
    i = standalone ? (nl == -1 ? n : (size_t)nl + 1) : end;
  }
  if (text.len > 0 && ok) {
    Token t = {0}; t.kind = TK_TEXT; t.text.s = arena_strndup(a, text.p, text.len); t.text.n = text.len;
    if (!t.text.s || !tok_push(a, out, t)) ok = false;
  }
  free(text.p);
  if (!ok) { *err = (char *)"out of memory"; return false; }
  return true;
}

// Parse one template source to an AST root. NULL + *err on TemplateError.
static TNode *parse_template(Arena *a, const char *tname, const char *src, size_t n, char **err) {
  TokList toks;
  if (!tokenize(a, tname, src, n, &toks, err)) return NULL;
  TNode *root = tnode_new(a, N_ROOT);
  if (!root) { *err = (char *)"out of memory"; return NULL; }
  // stack frames: node + sel (0 = kids, 1 = kids2)
  typedef struct { TNode *node; int sel; } Frame;
  Frame *st = (Frame *)malloc(64 * sizeof(Frame));
  if (!st) { *err = (char *)"out of memory"; return NULL; }
  size_t nst = 0, capst = 64;
  st[nst++] = (Frame){ root, 0 };
  bool ok = true;
  for (size_t ti = 0; ti < toks.n && ok; ti++) {
    Token *tok = &toks.toks[ti];
    Frame *top = &st[nst - 1];
    TNode *dst = top->sel == 0 ? top->node : top->node; // node whose list we append to
    (void)dst;
    bool second = top->sel == 1;
    if (tok->kind == TK_TEXT) {
      TNode *nd = tnode_new(a, N_TEXT);
      if (!nd || !tnode_kid(a, top->node, nd, second)) { ok = false; break; }
      // fix: text must be set on the node BEFORE appending is fine either way
      nd->text = tok->text;
    } else if (tok->kind == TK_VAR || tok->kind == TK_RAW) {
      TNode *nd = tnode_new(a, tok->kind == TK_VAR ? N_VAR : N_RAW);
      if (!nd) { ok = false; break; }
      nd->path = tok->path; nd->npath = tok->npath;
      if (!tnode_kid(a, top->node, nd, second)) { ok = false; break; }
    } else {
      // tag
      const char *tag = tok->tag.s;
      size_t tn = tok->tag.n;
      // FOR_RE: ^for ([A-Za-z_][A-Za-z0-9_]*) in (\S+)$
      bool handled = false;
      if (tn >= 4 && memcmp(tag, "for ", 4) == 0) {
        size_t p = 4;
        if (p < tn && is_ident_start(tag[p])) {
          size_t vs = p;
          while (p < tn && is_ident_char(tag[p])) p++;
          if (p + 4 <= tn && memcmp(tag + p, " in ", 4) == 0) {
            const char *rp = tag + p + 4;
            size_t rn = tn - (p + 4);
            bool nospace = rn > 0;
            for (size_t q = 0; q < rn && nospace; q++) {
              const char *qq = rp + q; uint32_t cp = utf8_decode(&qq, rp + rn);
              if (is_js_ws_cp(cp)) nospace = false;
              q = (size_t)(qq - rp) - 1;
            }
            if (nospace) {
              Str *path = NULL; size_t npath = 0;
              if (!path_of(a, rp, rn, tname, &path, &npath, err)) { free(st); return NULL; }
              TNode *nd = tnode_new(a, N_FOR);
              if (!nd) { ok = false; break; }
              nd->loop_name.s = (char *)tag + vs; nd->loop_name.n = p - vs;
              nd->path = path; nd->npath = npath;
              if (!tnode_kid(a, top->node, nd, second)) { ok = false; break; }
              if (nst == capst) {
                capst *= 2;
                Frame *ns = (Frame *)realloc(st, capst * sizeof(Frame));
                if (!ns) { ok = false; break; }
                st = ns;
              }
              st[nst++] = (Frame){ nd, 0 };
              handled = true;
            }
          }
        }
      }
      if (handled) continue;
      // IF_RE: ^if (not )?(\S+)$
      if (!handled && tn >= 3 && memcmp(tag, "if ", 3) == 0) {
        size_t p = 3;
        bool not = false;
        if (p + 4 <= tn && memcmp(tag + p, "not ", 4) == 0) { not = true; p += 4; }
        const char *rp = tag + p;
        size_t rn = tn - p;
        bool nospace = rn > 0;
        for (size_t q = 0; q < rn && nospace; q++) {
          const char *qq = rp + q; uint32_t cp = utf8_decode(&qq, rp + rn);
          if (is_js_ws_cp(cp)) nospace = false;
          q = (size_t)(qq - rp) - 1;
        }
        if (nospace) {
          Str *path = NULL; size_t npath = 0;
          if (!path_of(a, rp, rn, tname, &path, &npath, err)) { free(st); return NULL; }
          TNode *nd = tnode_new(a, N_IF);
          if (!nd) { ok = false; break; }
          nd->not = not;
          nd->path = path; nd->npath = npath;
          if (!tnode_kid(a, top->node, nd, second)) { ok = false; break; }
          if (nst == capst) {
            capst *= 2;
            Frame *ns = (Frame *)realloc(st, capst * sizeof(Frame));
            if (!ns) { ok = false; break; }
            st = ns;
          }
          st[nst++] = (Frame){ nd, 0 };
          handled = true;
        }
      }
      if (handled) continue;
      if (tn == 4 && memcmp(tag, "else", 4) == 0) {
        if (top->node->kind != N_IF || top->node->nkids2 > 0 || top->sel == 1) {
          char tmp[256];
          snprintf(tmp, sizeof tmp, "template \"%s\": an else outside an if", tname);
          *err = arena_strndup(a, tmp, strlen(tmp));
          free(st);
          return NULL;
        }
        top->sel = 1;
        handled = true;
      } else if ((tn == 6 && memcmp(tag, "endfor", 6) == 0) || (tn == 5 && memcmp(tag, "endif", 5) == 0)) {
        bool is_for = (tn == 6);
        if (top->node->kind != (is_for ? N_FOR : N_IF)) {
          char tmp[256];
          snprintf(tmp, sizeof tmp, "template \"%s\": an %s with no open %s", tname,
                   is_for ? "endfor" : "endif", is_for ? "for" : "if");
          *err = arena_strndup(a, tmp, strlen(tmp));
          free(st);
          return NULL;
        }
        nst--;
        handled = true;
      } else if (tn >= 10 && memcmp(tag, "include \"", 9) == 0 && tag[tn - 1] == '"') {
        // INCLUDE_RE: ^include "([a-z0-9-]+)"$
        const char *nm = tag + 9;
        size_t nn = tn - 10;
        bool good = nn > 0;
        for (size_t q = 0; q < nn && good; q++) {
          char c = nm[q];
          if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) good = false;
        }
        if (good) {
          TNode *nd = tnode_new(a, N_INCLUDE);
          if (!nd) { ok = false; break; }
          nd->inc_name.s = (char *)nm; nd->inc_name.n = nn;
          if (!tnode_kid(a, top->node, nd, second)) { ok = false; break; }
          handled = true;
        }
      }
      if (!handled) {
        char tmp[1024];
        snprintf(tmp, sizeof tmp, "template \"%s\": {%% %.*s %%} is not a tag", tname, (int)tn, tag);
        *err = arena_strndup(a, tmp, strlen(tmp));
        free(st);
        return NULL;
      }
    }
  }
  if (ok && nst > 1) {
    const char *kind = st[nst - 1].node->kind == N_FOR ? "for" : "if";
    char tmp[256];
    snprintf(tmp, sizeof tmp, "template \"%s\": a %s is not closed", tname, kind);
    *err = arena_strndup(a, tmp, strlen(tmp));
    free(st);
    return NULL;
  }
  free(st);
  if (!ok) { *err = (char *)"out of memory"; return NULL; }
  return root;
}

// ---- template sets + invariant detection ----
typedef struct { Str *items; size_t n, cap; } StrSet;
static bool set_add(StrSet *s, Str v) {
  for (size_t i = 0; i < s->n; i++) if (s->items[i].n == v.n && memcmp(s->items[i].s, v.s, v.n) == 0) return true;
  if (s->n == s->cap) {
    size_t nc = s->cap ? s->cap * 2 : 16;
    Str *ni = (Str *)realloc(s->items, nc * sizeof(Str));
    if (!ni) return false;
    s->items = ni; s->cap = nc;
  }
  s->items[s->n++] = v;
  return true;
}
static bool set_has(StrSet *s, const char *p, size_t n) {
  for (size_t i = 0; i < s->n; i++) if (s->items[i].n == n && memcmp(s->items[i].s, p, n) == 0) return true;
  return false;
}
static void set_del(StrSet *s, const char *p, size_t n) {
  for (size_t i = 0; i < s->n; i++) {
    if (s->items[i].n == n && memcmp(s->items[i].s, p, n) == 0) {
      memmove(&s->items[i], &s->items[i + 1], (s->n - i - 1) * sizeof(Str));
      s->n--;
      return;
    }
  }
}
static void collect_roots(TNode **nodes, size_t n, Str *bound, size_t nbound, StrSet *roots, StrSet *incs, Arena *a) {
  for (size_t i = 0; i < n; i++) {
    TNode *nd = nodes[i];
    if (nd->kind == N_VAR || nd->kind == N_RAW || nd->kind == N_IF) {
      if (nd->npath > 0) {
        bool b = false;
        for (size_t k = 0; k < nbound; k++) if (bound[k].n == nd->path[0].n && memcmp(bound[k].s, nd->path[0].s, nd->path[0].n) == 0) { b = true; break; }
        if (!b) set_add(roots, nd->path[0]);
      }
    } else if (nd->kind == N_FOR) {
      if (nd->npath > 0) {
        bool b = false;
        for (size_t k = 0; k < nbound; k++) if (bound[k].n == nd->path[0].n && memcmp(bound[k].s, nd->path[0].s, nd->path[0].n) == 0) { b = true; break; }
        if (!b) set_add(roots, nd->path[0]);
      }
      Str *b2 = (Str *)arena_alloc(a, (nbound + 1) * sizeof(Str));
      if (b2) {
        memcpy(b2, bound, nbound * sizeof(Str));
        b2[nbound] = nd->loop_name;
        collect_roots(nd->kids, nd->nkids, b2, nbound + 1, roots, incs, a);
      }
    }
    if (nd->kind == N_IF) {
      collect_roots(nd->kids, nd->nkids, bound, nbound, roots, incs, a);
      if (nd->kids2) collect_roots(nd->kids2, nd->nkids2, bound, nbound, roots, incs, a);
    } else if (nd->kind == N_INCLUDE) {
      set_add(incs, nd->inc_name);
    }
  }
}

typedef struct {
  Arena *arena;
  char **names; TNode **trees; size_t n, cap; // parsed templates (malloc arrays)
  HMap inv_memo;   // name -> (void*)(intptr_t)(ok?2:1)
  HMap inv_set;    // name -> (void*)1
  HMap inv_cache;  // name -> Str* (rendered)
} TplCtx;
static TNode *tpl_get(TplCtx *tc, const char *name, size_t nlen) {
  for (size_t i = 0; i < tc->n; i++)
    if (strlen(tc->names[i]) == nlen && memcmp(tc->names[i], name, nlen) == 0) return tc->trees[i];
  return NULL;
}
static bool tpl_put(TplCtx *tc, const char *name, TNode *tree) {
  if (tc->n == tc->cap) {
    size_t nc = tc->cap ? tc->cap * 2 : 32;
    char **nn = (char **)realloc(tc->names, nc * sizeof(char *));
    TNode **nt = (TNode **)realloc(tc->trees, nc * sizeof(TNode *));
    if (!nn || !nt) return false;
    tc->names = nn; tc->trees = nt; tc->cap = nc;
  }
  tc->names[tc->n] = (char *)name;
  tc->trees[tc->n] = tree;
  tc->n++;
  return true;
}
static bool is_invariant(TplCtx *tc, const char *name, size_t nlen, StrSet *seen) {
  void *m = hmap_get(&tc->inv_memo, name, nlen);
  if (m) return (intptr_t)m == 2;
  if (set_has(seen, name, nlen)) return true;
  TNode *tree = tpl_get(tc, name, nlen);
  if (!tree) return false;
  set_add(seen, (Str){ (char *)name, nlen });
  StrSet roots = {0}, incs = {0};
  collect_roots(tree->kids, tree->nkids, NULL, 0, &roots, &incs, tc->arena);
  bool ok = true;
  for (size_t i = 0; i < roots.n && ok; i++) {
    Str r = roots.items[i];
    if (!((r.n == 4 && memcmp(r.s, "site", 4) == 0) || (r.n == 4 && memcmp(r.s, "tags", 4) == 0) ||
          (r.n == 6 && memcmp(r.s, "recent", 6) == 0))) ok = false;
  }
  if (ok) for (size_t i = 0; i < incs.n && ok; i++)
    if (!is_invariant(tc, incs.items[i].s, incs.items[i].n, seen)) ok = false;
  set_del(seen, name, nlen);
  hmap_put(&tc->inv_memo, name, nlen, (void *)(intptr_t)(ok ? 2 : 1));
  free(roots.items); free(incs.items);
  return ok;
}

// ---- template interpreter ----
static Val *g_val(Arena *a, Val *v, const char *key, size_t klen) {
  if (!v) return NULL;
  if (v->t == V_OBJ) return obj_get(v, key, klen);
  if (v->t == V_ARR) {
    if (klen == 6 && memcmp(key, "length", 6) == 0) return val_num(a, (double)v->nitems);
    bool digits = klen > 0;
    for (size_t i = 0; i < klen && digits; i++) if (key[i] < '0' || key[i] > '9') digits = false;
    if (digits) {
      unsigned long idx = 0;
      for (size_t i = 0; i < klen; i++) {
        idx = idx * 10 + (unsigned long)(key[i] - '0');
        if (idx >= v->nitems && i + 1 < klen) return NULL;
      }
      if (idx < v->nitems) return v->items[idx];
    }
    return NULL;
  }
  return NULL;
}
typedef struct { Val *loop_obj; Str name; Val *elem; } LoopCtx;
typedef struct {
  TplCtx *tc;
  Val *V, *L;
  LoopCtx *loops; size_t nloops, caploops;
  int depth;
  Arena *arena;
  Buf *out;
  bool failed;
} Interp;
static bool is_v_root(Str s) {
  return (s.n == 4 && (memcmp(s.s, "site", 4) == 0 || memcmp(s.s, "tags", 4) == 0)) ||
         (s.n == 6 && memcmp(s.s, "recent", 6) == 0) ||
         (s.n == 4 && memcmp(s.s, "page", 4) == 0) ||
         (s.n == 5 && (memcmp(s.s, "posts", 5) == 0 || memcmp(s.s, "pager", 5) == 0)) ||
         (s.n == 3 && memcmp(s.s, "tag", 3) == 0);
}
static Val *interp_resolve(Interp *it, Str *path, size_t npath) {
  Val *expr = NULL;
  if (npath > 0 && path[0].n == 4 && memcmp(path[0].s, "loop", 4) == 0) {
    if (it->nloops == 0) return NULL;
    LoopCtx *ctx = &it->loops[it->nloops - 1];
    if (npath == 1) return ctx->loop_obj;
    if (npath == 2) {
      if (path[1].n == 5 && memcmp(path[1].s, "index", 5) == 0) return obj_get(ctx->loop_obj, "index", 5);
      if (path[1].n == 5 && memcmp(path[1].s, "first", 5) == 0) return obj_get(ctx->loop_obj, "first", 5);
      if (path[1].n == 4 && memcmp(path[1].s, "last", 4) == 0) return obj_get(ctx->loop_obj, "last", 4);
      if (path[1].n == 6 && memcmp(path[1].s, "length", 6) == 0) return obj_get(ctx->loop_obj, "length", 6);
    }
    expr = ctx->loop_obj;
    for (size_t k = 1; k < npath; k++) {
      expr = g_val(it->arena, expr, path[k].s, path[k].n);
      if (!expr) return NULL;
    }
    return expr;
  }
  for (size_t s = it->nloops; s-- > 0;) {
    if (it->loops[s].name.n == path[0].n && memcmp(it->loops[s].name.s, path[0].s, path[0].n) == 0) {
      expr = it->loops[s].elem;
      goto chain;
    }
  }
  if (is_v_root(path[0])) expr = obj_get(it->V, path[0].s, path[0].n);
  else expr = obj_get(it->L, path[0].s, path[0].n);
chain:
  for (size_t k = 1; k < npath; k++) {
    expr = g_val(it->arena, expr, path[k].s, path[k].n);
    if (!expr) return NULL;
  }
  return expr;
}
static bool text_of_buf(Interp *it, Val *v, Buf *out) {
  if (!v) return true;
  switch (v->t) {
    case V_STR: return buf_putn(out, v->str.s, v->str.n);
    case V_NUM: { char nb[64]; js_num_str(v->d, nb, sizeof nb); return buf_puts(out, nb); }
    case V_BOOL: return buf_puts(out, v->b ? "true" : "false");
    case V_ARR:
      for (size_t i = 0; i < v->nitems; i++) {
        if (i && !buf_puts(out, ", ")) return false;
        if (!text_of_buf(it, v->items[i], out)) return false;
      }
      return true;
    default: return true; // null / object -> ''
  }
}
static bool truthy_val(Val *v) {
  if (!v) return false;
  if (v->t == V_ARR) return v->nitems > 0;
  if (v->t == V_BOOL) return v->b;
  if (v->t == V_STR) return v->str.n > 0;
  if (v->t == V_NUM) return v->d != 0;
  if (v->t == V_NULL) return false;
  return true; // object
}
static bool interp_nodes(Interp *it, TNode **nodes, size_t n);
static bool interp_nodes(Interp *it, TNode **nodes, size_t n) {
  for (size_t i = 0; i < n && !it->failed; i++) {
    TNode *nd = nodes[i];
    switch (nd->kind) {
      case N_TEXT:
        if (!buf_putn(it->out, nd->text.s, nd->text.n)) it->failed = true;
        break;
      case N_VAR: {
        Val *v = interp_resolve(it, nd->path, nd->npath);
        Buf tmp = {0};
        if (!text_of_buf(it, v, &tmp)) { free(tmp.p); it->failed = true; break; }
        if (!buf_escape(it->out, tmp.p ? tmp.p : "", tmp.len)) it->failed = true;
        free(tmp.p);
        break;
      }
      case N_RAW: {
        Val *v = interp_resolve(it, nd->path, nd->npath);
        if (!text_of_buf(it, v, it->out)) it->failed = true;
        break;
      }
      case N_IF: {
        Val *v = interp_resolve(it, nd->path, nd->npath);
        bool t = truthy_val(v);
        if (nd->not) t = !t;
        if (t) { if (!interp_nodes(it, nd->kids, nd->nkids)) it->failed = true; }
        else if (nd->kids2) { if (!interp_nodes(it, nd->kids2, nd->nkids2)) it->failed = true; }
        break;
      }
      case N_FOR: {
        Val *a = interp_resolve(it, nd->path, nd->npath);
        if (!a || a->t != V_ARR) break;
        if (it->nloops == it->caploops) {
          size_t nc = it->caploops ? it->caploops * 2 : 8;
          LoopCtx *nl = (LoopCtx *)realloc(it->loops, nc * sizeof(LoopCtx));
          if (!nl) { it->failed = true; break; }
          it->loops = nl; it->caploops = nc;
        }
        Val *lp = val_new(it->arena, V_OBJ);
        if (!lp) { it->failed = true; break; }
        Val *old_lvar = obj_get(it->L, nd->loop_name.s, nd->loop_name.n);
        LoopCtx *ctx = &it->loops[it->nloops++];
        ctx->loop_obj = lp; ctx->name = nd->loop_name; ctx->elem = NULL;
        size_t nn = a->nitems;
        Val *num_len = val_num(it->arena, (double)nn);
        if (!num_len || !obj_set(it->arena, lp, "length", 6, num_len)) { it->failed = true; it->nloops--; break; }
        for (size_t k = 0; k < nn && !it->failed; k++) {
          Val *el = a->items[k];
          ctx->elem = el;
          Val *num_i = val_num(it->arena, (double)(k + 1));
          Val *b_first = val_bool(it->arena, k == 0);
          Val *b_last = val_bool(it->arena, k == nn - 1);
          if (!num_i || !b_first || !b_last ||
              !obj_set(it->arena, lp, "index", 5, num_i) ||
              !obj_set(it->arena, lp, "first", 5, b_first) ||
              !obj_set(it->arena, lp, "last", 4, b_last) ||
              !obj_set(it->arena, it->L, nd->loop_name.s, nd->loop_name.n, el)) { it->failed = true; break; }
          if (!interp_nodes(it, nd->kids, nd->nkids)) it->failed = true;
        }
        it->nloops--;
        if (old_lvar) { if (!obj_set(it->arena, it->L, nd->loop_name.s, nd->loop_name.n, old_lvar)) it->failed = true; }
        else obj_del(it->L, nd->loop_name.s, nd->loop_name.n);
        break;
      }
      case N_INCLUDE: {
        if (hmap_get(&it->tc->inv_set, nd->inc_name.s, nd->inc_name.n)) {
          Str *cached = (Str *)hmap_get(&it->tc->inv_cache, nd->inc_name.s, nd->inc_name.n);
          if (cached && !buf_putn(it->out, cached->s, cached->n)) it->failed = true;
        } else if (it->depth < 8) {
          TNode *sub = tpl_get(it->tc, nd->inc_name.s, nd->inc_name.n);
          if (sub) {
            it->depth++;
            if (!interp_nodes(it, sub->kids, sub->nkids)) it->failed = true;
            it->depth--;
          }
        }
        break;
      }
      default: break;
    }
  }
  return !it->failed;
}

// ---- run() pipeline ----
typedef struct Source Source;
typedef struct Tag Tag;
struct Source {
  Str path;
  Str text;      // normalized content (for .md sources)
  Val *data;
  Str body;
  bool post;
  Str layout;
  Str slug, out_dir, url;
  char *html; size_t html_len;
  Val *toc;
  Str excerpt;
  Source *newer, *older;
  Tag **tagrefs; size_t ntagrefs, captagrefs;
  Val *card;
};
struct Tag {
  Str name, slug, url;
  Source **posts; size_t nposts, capposts;
};
typedef struct {
  Arena *arena;
  Val *input;
  Str base;
  HMap slug_cache;   // text -> Str* (slug)
  HMap link_cache;   // dir\0target -> Str* (resolved)
  HMap by_source;    // path -> Source*
  HMap taken;        // dir\0slug -> (void*)1
  HMap static_set;   // path -> (void*)1
  HMap tag_map;      // slug -> Tag*
  Str *broken; size_t nbroken, capbroken;
  Str *report; size_t nreport, capreport;
  Str cur_dir, cur_path;
  TplCtx tpl;
  Source **sources; size_t nsources, capsources;
  bool failed;
} RunCtx;
static bool strlist_push(Arena *a, Str **arr, size_t *n, size_t *cap, Str v) {
  if (*n == *cap) {
    size_t nc = *cap ? *cap * 2 : 32;
    Str *na = (Str *)arena_alloc(a, nc * sizeof(Str));
    if (!na) return false;
    memcpy(na, *arr, *n * sizeof(Str));
    *arr = na; *cap = nc;
  }
  (*arr)[(*n)++] = v;
  return true;
}
static bool report_push(RunCtx *rc, const char *s, size_t n) {
  char *c = arena_strndup(rc->arena, s, n);
  if (!c) return false;
  return strlist_push(rc->arena, &rc->report, &rc->nreport, &rc->capreport, (Str){ c, n });
}
static bool report_pushf(RunCtx *rc, const char *fmt, const char *a1, const char *a2) {
  char tmp[1024];
  // fmt contains two %s
  snprintf(tmp, sizeof tmp, fmt, a1, a2);
  return report_push(rc, tmp, strlen(tmp));
}
static bool broken_push(RunCtx *rc, const char *s, size_t n) {
  for (size_t i = 0; i < rc->nbroken; i++)
    if (rc->broken[i].n == n && memcmp(rc->broken[i].s, s, n) == 0) return true;
  char *c = arena_strndup(rc->arena, s, n);
  if (!c) return false;
  return strlist_push(rc->arena, &rc->broken, &rc->nbroken, &rc->capbroken, (Str){ c, n });
}
static int cmp_str_qsort(const void *pa, const void *pb) {
  const Str *a = (const Str *)pa, *b = (const Str *)pb;
  return cmp_str(*a, *b);
}
static int cmp_val_path(const void *pa, const void *pb) {
  Val *a = *(Val **)pa, *b = *(Val **)pb;
  Val *ap = obj_gets(a, "path"), *bp = obj_gets(b, "path");
  return cmp_str(ap->str, bp->str);
}

// C link resolver for md_render (ud = RunCtx*). Returns malloc'd string.
static bool is_scheme_target(const char *t, size_t n) {
  if (n == 0 || !((t[0] >= 'A' && t[0] <= 'Z') || (t[0] >= 'a' && t[0] <= 'z'))) return false;
  size_t i = 1;
  while (i < n) {
    char c = t[i];
    if (c == ':') return true;
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '.' || c == '-')) return false;
    i++;
  }
  return false;
}
static char *c_link_fn(void *ud, const char *target, size_t tlen, size_t *out_len) {
  RunCtx *rc = (RunCtx *)ud;
  if (is_scheme_target(target, tlen) || (tlen > 0 && (target[0] == '/' || target[0] == '#'))) {
    char *r = (char *)malloc(tlen + 1);
    if (!r) return NULL;
    memcpy(r, target, tlen); r[tlen] = 0; *out_len = tlen;
    return r;
  }
  // cache key: cur_dir + '\0' + target (arena copy for the map)
  size_t klen = rc->cur_dir.n + 1 + tlen;
  char *key = (char *)arena_alloc(rc->arena, klen + 1);
  if (!key) return NULL;
  memcpy(key, rc->cur_dir.s, rc->cur_dir.n);
  key[rc->cur_dir.n] = '\0';
  memcpy(key + rc->cur_dir.n + 1, target, tlen);
  key[klen] = 0;
  Str *hit = (Str *)hmap_get(&rc->link_cache, key, klen);
  if (hit) {
    char *r = (char *)malloc(hit->n + 1);
    if (!r) return NULL;
    memcpy(r, hit->s, hit->n); r[hit->n] = 0; *out_len = hit->n;
    return r;
  }
  // split at '#'
  size_t hash = tlen;
  for (size_t i = 0; i < tlen; i++) if (target[i] == '#') { hash = i; break; }
  const char *rel = target;
  size_t reln = hash;
  const char *frag = target + hash;
  size_t fragn = tlen - hash;
  Str path = {0};
  bool has_path = false;
  if (reln > 0) has_path = resolve_path_c(rc->cur_dir.s, rc->cur_dir.n, rel, reln, rc->arena, &path);
  char *out = NULL;
  bool ok = false;
  if (has_path) {
    Source *s = (Source *)hmap_get(&rc->by_source, path.s, path.n);
    if (s) {
      // out = s.url + frag
      size_t n = s->url.n + fragn;
      char *r = (char *)arena_alloc(rc->arena, n + 1);
      if (!r) return NULL;
      memcpy(r, s->url.s, s->url.n);
      memcpy(r + s->url.n, frag, fragn);
      r[n] = 0;
      Str *ent = (Str *)arena_alloc(rc->arena, sizeof(Str));
      if (!ent) return NULL;
      ent->s = r; ent->n = n;
      if (!hmap_put(&rc->link_cache, key, klen, ent)) return NULL;
      out = (char *)malloc(n + 1);
      if (!out) return NULL;
      memcpy(out, r, n); out[n] = 0; *out_len = n;
      ok = true;
    } else if (hmap_get(&rc->static_set, path.s, path.n)) {
      // out = base + '/' + path + frag
      size_t n = rc->base.n + 1 + path.n + fragn;
      char *r = (char *)arena_alloc(rc->arena, n + 1);
      if (!r) return NULL;
      memcpy(r, rc->base.s, rc->base.n);
      r[rc->base.n] = '/';
      memcpy(r + rc->base.n + 1, path.s, path.n);
      memcpy(r + rc->base.n + 1 + path.n, frag, fragn);
      r[n] = 0;
      Str *ent = (Str *)arena_alloc(rc->arena, sizeof(Str));
      if (!ent) return NULL;
      ent->s = r; ent->n = n;
      if (!hmap_put(&rc->link_cache, key, klen, ent)) return NULL;
      out = (char *)malloc(n + 1);
      if (!out) return NULL;
      memcpy(out, r, n); out[n] = 0; *out_len = n;
      ok = true;
    }
  }
  if (!ok) {
    // broken-link ${cur_path} -> ${target}
    Buf b = {0};
    buf_puts(&b, "broken-link ");
    buf_putn(&b, rc->cur_path.s, rc->cur_path.n);
    buf_puts(&b, " -> ");
    buf_putn(&b, target, tlen);
    broken_push(rc, b.p ? b.p : "", b.len);
    free(b.p);
    out = (char *)malloc(tlen + 1);
    if (!out) return NULL;
    memcpy(out, target, tlen); out[tlen] = 0; *out_len = tlen;
  }
  return out;
}
static char *c_slug_fn(void *ud, const char *s, size_t n, size_t *out_len) {
  RunCtx *rc = (RunCtx *)ud;
  size_t slen = 0;
  char *slug = slugify_c(rc->arena, &rc->slug_cache, s, n, &slen);
  if (!slug) return NULL;
  char *r = (char *)malloc(slen + 1);
  if (!r) return NULL;
  memcpy(r, slug, slen); r[slen] = 0; *out_len = slen;
  return r;
}
static char *unique_slug_c(RunCtx *rc, Str dir, const char *slug, size_t slen) {
  // try slug, slug-2, ...
  for (int num = 1; ; num++) {
    Buf b = {0};
    buf_putn(&b, slug, slen);
    if (num > 1) { char nb[24]; int w = snprintf(nb, sizeof nb, "-%d", num); buf_putn(&b, nb, (size_t)w); }
    size_t klen = dir.n + 1 + b.len;
    char *key = (char *)arena_alloc(rc->arena, klen + 1);
    if (!key) { free(b.p); return NULL; }
    memcpy(key, dir.s, dir.n);
    key[dir.n] = '\0';
    memcpy(key + dir.n + 1, b.p, b.len);
    key[klen] = 0;
    if (!hmap_get(&rc->taken, key, klen)) {
      if (!hmap_put(&rc->taken, key, klen, (void *)1)) { free(b.p); return NULL; }
      char *r = arena_strndup(rc->arena, b.p, b.len);
      free(b.p);
      return r;
    }
    free(b.p);
  }
}

// ---- render helpers ----
static int render_tpl(RunCtx *rc, const char *name, size_t nlen, Val *V, Buf *out) {
  TNode *tree = tpl_get(&rc->tpl, name, nlen);
  if (!tree) return 0;
  Interp it;
  memset(&it, 0, sizeof it);
  it.tc = &rc->tpl; it.V = V; it.arena = rc->arena; it.out = out; it.depth = 0;
  it.L = val_new(rc->arena, V_OBJ);
  if (!it.L) return -1;
  bool ok = interp_nodes(&it, tree->kids, tree->nkids);
  free(it.loops);
  return ok ? 1 : -1;
}
static void v_set_or_del(RunCtx *rc, Val *V, const char *key, Val *v) {
  if (v) obj_sets(rc->arena, V, key, v);
  else obj_del(V, key, strlen(key));
}
static Val *link_of(RunCtx *rc, Source *s) {
  if (!s) return val_new(rc->arena, V_NULL);
  Val *o = val_new(rc->arena, V_OBJ);
  if (!o) return NULL;
  Val *title = obj_gets(s->data, "title");
  Val *date = obj_gets(s->data, "date");
  Val *url = val_str(rc->arena, s->url.s, s->url.n);
  Val *dstr = date ? date : val_cstr(rc->arena, "");
  char ld[64] = "";
  if (date) long_date(date->str.s, ld, sizeof ld);
  Val *ldv = val_cstr(rc->arena, ld);
  if (!title || !url || !dstr || !ldv) return NULL;
  if (!obj_sets(rc->arena, o, "title", title) || !obj_sets(rc->arena, o, "url", url) ||
      !obj_sets(rc->arena, o, "date", dstr) || !obj_sets(rc->arena, o, "date_long", ldv)) return NULL;
  return o;
}
static Val *card_of(RunCtx *rc, Source *s) {
  if (s->card) return s->card;
  Val *c = val_new(rc->arena, V_OBJ);
  if (!c) return NULL;
  Val *title = obj_gets(s->data, "title");
  Val *date = obj_gets(s->data, "date");
  Val *summary = obj_gets(s->data, "summary");
  Val *url = val_str(rc->arena, s->url.s, s->url.n);
  Val *dstr = date ? date : val_cstr(rc->arena, "");
  char ld[64] = "";
  if (date) long_date(date->str.s, ld, sizeof ld);
  Val *ldv = val_cstr(rc->arena, ld);
  Val *sstr = summary ? summary : val_cstr(rc->arena, "");
  Val *ex = val_str(rc->arena, s->excerpt.s, s->excerpt.n);
  long rm = words_of(s->body.s, s->body.n);
  rm = (rm + 199) / 200;
  if (rm < 1) rm = 1;
  Val *rmv = val_num(rc->arena, (double)rm);
  Val *tags = val_new(rc->arena, V_ARR);
  if (!title || !url || !dstr || !ldv || !sstr || !ex || !rmv || !tags) return NULL;
  for (size_t i = 0; i < s->ntagrefs; i++) {
    Tag *t = s->tagrefs[i];
    Val *to = val_new(rc->arena, V_OBJ);
    if (!to) return NULL;
    Val *nm = val_str(rc->arena, t->name.s, t->name.n);
    Val *sl = val_str(rc->arena, t->slug.s, t->slug.n);
    Val *uu = val_str(rc->arena, t->url.s, t->url.n);
    if (!nm || !sl || !uu) return NULL;
    if (!obj_sets(rc->arena, to, "name", nm) || !obj_sets(rc->arena, to, "slug", sl) ||
        !obj_sets(rc->arena, to, "url", uu) || !arr_push(rc->arena, tags, to)) return NULL;
  }
  if (!obj_sets(rc->arena, c, "title", title) || !obj_sets(rc->arena, c, "url", url) ||
      !obj_sets(rc->arena, c, "date", dstr) || !obj_sets(rc->arena, c, "date_long", ldv) ||
      !obj_sets(rc->arena, c, "summary", sstr) || !obj_sets(rc->arena, c, "excerpt", ex) ||
      !obj_sets(rc->arena, c, "reading_minutes", rmv) || !obj_sets(rc->arena, c, "tags", tags)) return NULL;
  s->card = c;
  return c;
}
static Val *page_of(RunCtx *rc, Source *s) {
  Val *c = card_of(rc, s);
  if (!c) return NULL;
  if (!obj_has(c, "content", 7)) {
    Val *sl = val_str(rc->arena, s->slug.s, s->slug.n);
    Val *ct = val_str(rc->arena, s->html, s->html_len);
    Val *nw = link_of(rc, s->newer);
    Val *ol = link_of(rc, s->older);
    Val *ip = val_bool(rc->arena, s->post);
    if (!sl || !ct || !nw || !ol || !ip) return NULL;
    if (!obj_sets(rc->arena, c, "slug", sl) || !obj_sets(rc->arena, c, "content", ct) ||
        !obj_sets(rc->arena, c, "toc", s->toc) || !obj_sets(rc->arena, c, "newer", nw) ||
        !obj_sets(rc->arena, c, "older", ol) || !obj_sets(rc->arena, c, "is_post", ip)) return NULL;
  }
  return c;
}
static Val *tag_of(RunCtx *rc, Tag *t) {
  Val *o = val_new(rc->arena, V_OBJ);
  if (!o) return NULL;
  Val *nm = val_str(rc->arena, t->name.s, t->name.n);
  Val *sl = val_str(rc->arena, t->slug.s, t->slug.n);
  Val *uu = val_str(rc->arena, t->url.s, t->url.n);
  Val *ct = val_num(rc->arena, (double)t->nposts);
  Val *ps = val_new(rc->arena, V_ARR);
  if (!nm || !sl || !uu || !ct || !ps) return NULL;
  for (size_t i = 0; i < t->nposts; i++) {
    Val *card = card_of(rc, t->posts[i]);
    if (!card || !arr_push(rc->arena, ps, card)) return NULL;
  }
  if (!obj_sets(rc->arena, o, "name", nm) || !obj_sets(rc->arena, o, "slug", sl) ||
      !obj_sets(rc->arena, o, "url", uu) || !obj_sets(rc->arena, o, "count", ct) ||
      !obj_sets(rc->arena, o, "posts", ps)) return NULL;
  return o;
}
static int cmp_post_qsort(const void *pa, const void *pb) {
  Source *a = *(Source **)pa, *b = *(Source **)pb;
  Val *ad = obj_gets(a->data, "date"), *bd = obj_gets(b->data, "date");
  int c = cmp_str(bd->str, ad->str);
  if (c) return c;
  return cmp_str(a->slug, b->slug);
}
static int cmp_tag_qsort(const void *pa, const void *pb) {
  Tag *a = *(Tag **)pa, *b = *(Tag **)pb;
  if (a->nposts != b->nposts) return a->nposts > b->nposts ? -1 : 1;
  return cmp_str(a->slug, b->slug);
}
static void inv_visit(RunCtx *rc, const char *name, size_t nlen, StrSet *seen, StrSet *order) {
  if (set_has(seen, name, nlen)) return;
  if (!hmap_get(&rc->tpl.inv_set, name, nlen)) return;
  set_add(seen, (Str){ (char *)name, nlen });
  TNode *tree = tpl_get(&rc->tpl, name, nlen);
  if (tree) {
    StrSet roots = {0}, incs = {0};
    collect_roots(tree->kids, tree->nkids, NULL, 0, &roots, &incs, rc->arena);
    for (size_t i = 0; i < incs.n; i++) inv_visit(rc, incs.items[i].s, incs.items[i].n, seen, order);
    free(roots.items); free(incs.items);
  }
  set_add(order, (Str){ (char *)name, nlen });
}

// Run one case. Returns false on any failure (caller falls back to JS).
static bool run_case(Arena *arena, Val *input, Val **out_answer) {
  RunCtx rc;
  memset(&rc, 0, sizeof rc);
  rc.arena = arena;
  rc.input = input;
  rc.tpl.arena = arena;
  bool ok = false;
  Val *answer = NULL;
  if (!hmap_init(&rc.slug_cache, 1024) || !hmap_init(&rc.link_cache, 1024) ||
      !hmap_init(&rc.by_source, 1024) || !hmap_init(&rc.taken, 256) ||
      !hmap_init(&rc.static_set, 256) || !hmap_init(&rc.tag_map, 256) ||
      !hmap_init(&rc.tpl.inv_memo, 64) || !hmap_init(&rc.tpl.inv_set, 64) ||
      !hmap_init(&rc.tpl.inv_cache, 64)) goto done;
  Val *site = obj_gets(input, "site");
  Val *templates = obj_gets(input, "templates");
  Val *files = obj_gets(input, "files");
  if (!site || !templates || templates->t != V_OBJ || !files || files->t != V_ARR) goto done;
  Val *base_url = obj_gets(site, "base_url");
  if (!base_url || base_url->t != V_STR) goto done;
  {
    size_t n = base_url->str.n;
    while (n > 0 && base_url->str.s[n - 1] == '/') n--;
    rc.base.s = base_url->str.s; rc.base.n = n;
  }
  // 1. Templates, sorted by name.
  bool prof2 = getenv("V4_PROFILE2") != NULL;
  double _t0 = 0; double _ph[4] = {0};
  if (prof2) _t0 = (double)clock() * 1000.0 / CLOCKS_PER_SEC;
  {
    Str *names = (Str *)arena_alloc(arena, templates->nobj * sizeof(Str));
    if (!names && templates->nobj > 0) goto done;
    for (size_t i = 0; i < templates->nobj; i++) names[i] = templates->keys[i];
    qsort(names, templates->nobj, sizeof(Str), cmp_str_qsort);
    for (size_t i = 0; i < templates->nobj; i++) {
      Val *src = obj_get(templates, names[i].s, names[i].n);
      char *err = NULL;
      TNode *tree = parse_template(arena, names[i].s, src->str.s, src->str.n, &err);
      if (!tree) {
        if (!report_push(&rc, err ? err : "template error", err ? strlen(err) : 14)) goto done;
      } else if (!tpl_put(&rc.tpl, names[i].s, tree)) goto done;
    }
  }
  // Invariant templates.
  for (size_t i = 0; i < rc.tpl.n; i++) {
    StrSet seen = {0};
    if (is_invariant(&rc.tpl, rc.tpl.names[i], strlen(rc.tpl.names[i]), &seen))
      hmap_put(&rc.tpl.inv_set, rc.tpl.names[i], strlen(rc.tpl.names[i]), (void *)1);
    free(seen.items);
  }
  // 2. Sources / statics (files sorted by path).
  Val **sorted_files = (Val **)arena_alloc(arena, (files->nitems ? files->nitems : 1) * sizeof(Val *));
  if (!sorted_files) goto done;
  memcpy(sorted_files, files->items, files->nitems * sizeof(Val *));
  qsort(sorted_files, files->nitems, sizeof(Val *), cmp_val_path);
  typedef struct { Str path, content; } StaticEnt;
  StaticEnt *statics = NULL; size_t nstatics = 0, capstatics = 0;
  for (size_t fi = 0; fi < files->nitems; fi++) {
    Val *f = sorted_files[fi];
    Val *path = obj_gets(f, "path");
    Val *content = obj_gets(f, "content");
    if (!path || !content || path->t != V_STR || content->t != V_STR) goto done;
    bool is_md = path->str.n >= 3 && memcmp(path->str.s + path->str.n - 3, ".md", 3) == 0;
    if (!is_md) {
      if (nstatics == capstatics) {
        size_t nc = capstatics ? capstatics * 2 : 32;
        StaticEnt *na = (StaticEnt *)arena_alloc(arena, nc * sizeof(StaticEnt));
        if (!na) goto done;
        memcpy(na, statics, nstatics * sizeof(StaticEnt));
        statics = na; capstatics = nc;
      }
      statics[nstatics].path = path->str;
      statics[nstatics].content = content->str;
      nstatics++;
      if (!hmap_put(&rc.static_set, path->str.s, path->str.n, (void *)1)) goto done;
      continue;
    }
    // normalize \r\n? -> \n
    const char *txt = content->str.s;
    size_t txtn = content->str.n;
    bool has_cr = false;
    for (size_t i = 0; i < txtn; i++) if (txt[i] == '\r') { has_cr = true; break; }
    char *norm = NULL;
    size_t normn = txtn;
    if (has_cr) {
      Buf b = {0};
      for (size_t i = 0; i < txtn; i++) {
        if (txt[i] == '\r') {
          if (!buf_putc(&b, '\n')) { free(b.p); goto done; }
          if (i + 1 < txtn && txt[i + 1] == '\n') i++;
        } else if (!buf_putc(&b, txt[i])) { free(b.p); goto done; }
      }
      norm = arena_strndup(arena, b.p ? b.p : "", b.len);
      normn = b.len;
      free(b.p);
      if (!norm) goto done;
    } else {
      norm = (char *)txt;
    }
    Source *s = (Source *)arena_alloc(arena, sizeof(Source));
    if (!s) goto done;
    memset(s, 0, sizeof *s);
    s->path.s = path->str.s; s->path.n = path->str.n;
    s->text.s = norm; s->text.n = normn;
    // front matter + checks
    FMResult fm;
    if (!parse_front_matter(arena, s->text.s, s->text.n, &fm)) {
      char tmp[1024];
      snprintf(tmp, sizeof tmp, "refused %.*s: %s", (int)s->path.n, s->path.s, fm.err ? fm.err : "error");
      if (!report_push(&rc, tmp, strlen(tmp))) goto done;
      continue;
    }
    s->data = fm.data;
    s->body = fm.body;
    s->post = (s->path.n >= 6 && memcmp(s->path.s, "posts/", 6) == 0);
    bool refused = false;
    if (s->path.n >= 5 && memcmp(s->path.s, "tags/", 5) == 0) {
      char tmp[1024];
      snprintf(tmp, sizeof tmp, "refused %.*s: tags/ is the generator's own", (int)s->path.n, s->path.s);
      if (!report_push(&rc, tmp, strlen(tmp))) goto done;
      refused = true;
    } else if (s->path.n >= 5 && memcmp(s->path.s, "page/", 5) == 0) {
      char tmp[1024];
      snprintf(tmp, sizeof tmp, "refused %.*s: page/ is the generator's own", (int)s->path.n, s->path.s);
      if (!report_push(&rc, tmp, strlen(tmp))) goto done;
      refused = true;
    }
    if (refused) continue;
    if (s->post && !obj_has(s->data, "date", 4)) {
      char tmp[1024];
      snprintf(tmp, sizeof tmp, "refused %.*s: a post needs a date", (int)s->path.n, s->path.s);
      if (!report_push(&rc, tmp, strlen(tmp))) goto done;
      continue;
    }
    Val *layout_v = obj_gets(s->data, "layout");
    const char *layout = NULL;
    size_t layout_n = 0;
    if (layout_v) { layout = layout_v->str.s; layout_n = layout_v->str.n; }
    else if (s->post) { layout = "post"; layout_n = 4; }
    else { layout = "page"; layout_n = 4; }
    s->layout.s = (char *)layout; s->layout.n = layout_n;
    if (!tpl_get(&rc.tpl, layout, layout_n)) {
      char tmp[1024];
      snprintf(tmp, sizeof tmp, "refused %.*s: layout \"%.*s\" is no template that parses",
               (int)s->path.n, s->path.s, (int)layout_n, layout);
      if (!report_push(&rc, tmp, strlen(tmp))) goto done;
      continue;
    }
    Val *draft = obj_gets(s->data, "draft");
    if (draft && draft->t == V_BOOL && draft->b) {
      char tmp[1024];
      snprintf(tmp, sizeof tmp, "draft %.*s", (int)s->path.n, s->path.s);
      if (!report_push(&rc, tmp, strlen(tmp))) goto done;
      continue;
    }
    if (rc.nsources == rc.capsources) {
      size_t nc = rc.capsources ? rc.capsources * 2 : 64;
      Source **na = (Source **)arena_alloc(arena, nc * sizeof(Source *));
      if (!na) goto done;
      memcpy(na, rc.sources, rc.nsources * sizeof(Source *));
      rc.sources = na; rc.capsources = nc;
    }
    rc.sources[rc.nsources++] = s;
  }
  // 3. Permalinks.
  {
    // seed dir '' with RESERVED
    const char *resv[] = { "posts", "tags", "page" };
    for (int i = 0; i < 3; i++) {
      size_t klen = 1 + strlen(resv[i]);
      char *key = (char *)arena_alloc(arena, klen + 1);
      if (!key) goto done;
      key[0] = '\0';
      memcpy(key + 1, resv[i], strlen(resv[i]));
      key[klen] = 0;
      if (!hmap_put(&rc.taken, key, klen, (void *)1)) goto done;
    }
    for (size_t i = 0; i < rc.nsources; i++) {
      Source *s = rc.sources[i];
      Str dir = s->post ? (Str){ (char *)"posts", 5 } : dir_of(s->path.s, s->path.n);
      Val *slug_v = obj_gets(s->data, "slug");
      Val *title_v = obj_gets(s->data, "title");
      char *slug = NULL;
      size_t slug_len = 0;
      if (slug_v) { slug = slug_v->str.s; slug_len = slug_v->str.n; }
      else {
        slug = slugify_c(arena, &rc.slug_cache, title_v->str.s, title_v->str.n, &slug_len);
        if (!slug) goto done;
      }
      char *uniq = unique_slug_c(&rc, dir, slug, slug_len);
      if (!uniq) goto done;
      size_t ulen = strlen(uniq);
      s->slug.s = uniq; s->slug.n = ulen;
      // outDir = dir=='' ? slug : dir/slug ; url = base/outDir/
      Buf b = {0};
      if (dir.n > 0) { if (!buf_putn(&b, dir.s, dir.n) || !buf_putc(&b, '/')) { free(b.p); goto done; } }
      if (!buf_putn(&b, uniq, ulen)) { free(b.p); goto done; }
      char *od = arena_strndup(arena, b.p ? b.p : "", b.len);
      size_t odn = b.len;
      free(b.p);
      if (!od) goto done;
      s->out_dir.s = od; s->out_dir.n = odn;
      Buf u = {0};
      if (!buf_putn(&u, rc.base.s, rc.base.n) || !buf_putc(&u, '/') ||
          !buf_putn(&u, od, odn) || !buf_putc(&u, '/')) { free(u.p); goto done; }
      char *uu = arena_strndup(arena, u.p ? u.p : "", u.len);
      size_t uun = u.len;
      free(u.p);
      if (!uu) goto done;
      s->url.s = uu; s->url.n = uun;
      if (!hmap_put(&rc.by_source, s->path.s, s->path.n, s)) goto done;
    }
  }
  if (prof2) { _ph[0] += (double)clock() * 1000.0 / CLOCKS_PER_SEC - _t0; _t0 = (double)clock() * 1000.0 / CLOCKS_PER_SEC; }
  // 4. Markdown.
  for (size_t i = 0; i < rc.nsources; i++) {
    Source *s = rc.sources[i];
    rc.cur_dir = dir_of(s->path.s, s->path.n);
    rc.cur_path = s->path;
    Ctx cx;
    memset(&cx, 0, sizeof cx);
    cx.use_js = false; cx.ud = &rc; cx.c_link = c_link_fn; cx.c_slug = c_slug_fn;
    MdOut out;
    if (!md_render(&cx, s->body.s, s->body.n, &out)) {
      md_out_free(&out);
      for (size_t k = 0; k < cx.nids; k++) free(cx.ids[k]);
      free(cx.ids);
      goto done;
    }
    // html = blocks joined + trailing '\n' when non-empty
    Buf hb = {0};
    if (!buf_putn(&hb, out.html.p ? out.html.p : "", out.html.len)) { md_out_free(&out); goto done; }
    if (out.nblocks > 0 && !buf_putc(&hb, '\n')) { free(hb.p); md_out_free(&out); goto done; }
    char *html = arena_strndup(arena, hb.p ? hb.p : "", hb.len);
    size_t hlen = hb.len;
    free(hb.p);
    if (!html) { md_out_free(&out); goto done; }
    s->html = html; s->html_len = hlen;
    Val *toc = val_new(arena, V_ARR);
    if (!toc) { md_out_free(&out); goto done; }
    for (size_t t = 0; t < out.ntoc; t++) {
      Val *e = val_new(arena, V_OBJ);
      Val *lv = val_num(arena, (double)out.toc_level[t]);
      Val *id = val_str(arena, out.toc_id[t], out.toc_idlen[t]);
      Val *hh = val_str(arena, out.toc_html[t], out.toc_htmllen[t]);
      if (!e || !lv || !id || !hh) { md_out_free(&out); goto done; }
      if (!obj_sets(arena, e, "level", lv) || !obj_sets(arena, e, "id", id) ||
          !obj_sets(arena, e, "html", hh) || !arr_push(arena, toc, e)) { md_out_free(&out); goto done; }
    }
    s->toc = toc;
    char *ex = arena_strndup(arena, out.excerpt.p ? out.excerpt.p : "", out.excerpt.len);
    size_t exn = out.excerpt.len;
    if (!ex) { md_out_free(&out); goto done; }
    s->excerpt.s = ex; s->excerpt.n = exn;
    md_out_free(&out);
    for (size_t k = 0; k < cx.nids; k++) free(cx.ids[k]);
    free(cx.ids);
  }
  if (prof2) { _ph[1] += (double)clock() * 1000.0 / CLOCKS_PER_SEC - _t0; _t0 = (double)clock() * 1000.0 / CLOCKS_PER_SEC; }
  // 5. Collections: posts sorted by date desc, slug asc.
  Source **posts = (Source **)arena_alloc(arena, (rc.nsources ? rc.nsources : 1) * sizeof(Source *));
  if (!posts) goto done;
  size_t nposts = 0;
  for (size_t i = 0; i < rc.nsources; i++) if (rc.sources[i]->post) posts[nposts++] = rc.sources[i];
  qsort(posts, nposts, sizeof(Source *), cmp_post_qsort);
  for (size_t k = 0; k < nposts; k++) {
    posts[k]->newer = k == 0 ? NULL : posts[k - 1];
    posts[k]->older = k == nposts - 1 ? NULL : posts[k + 1];
  }
  // 6. Tags. Phase A: posts only, in posts order (date desc, slug asc) —
  // the first name seen for a slug wins. Phase B: non-posts look up.
  {
    // ownTags helper: (name, slug) list deduped by slug, first name kept.
    typedef struct { Str name, slug; } TagName;
    for (size_t pi = 0; pi < nposts; pi++) {
      Source *s = posts[pi];
      Val *tags_v = obj_gets(s->data, "tags");
      size_t ntags_in = (tags_v && tags_v->t == V_ARR) ? tags_v->nitems : 0;
      TagName *list = (TagName *)arena_alloc(arena, (ntags_in ? ntags_in : 1) * sizeof(TagName));
      if (!list) goto done;
      size_t nlist = 0;
      for (size_t ti = 0; ti < ntags_in; ti++) {
        Val *tv = tags_v->items[ti];
        size_t slen = 0;
        char *slug = slugify_c(arena, &rc.slug_cache, tv->str.s, tv->str.n, &slen);
        if (!slug) goto done;
        bool dup = false;
        for (size_t q = 0; q < nlist; q++) if (list[q].slug.n == slen && memcmp(list[q].slug.s, slug, slen) == 0) { dup = true; break; }
        if (dup) continue;
        list[nlist].name = tv->str;
        list[nlist].slug.s = slug; list[nlist].slug.n = slen;
        nlist++;
      }
      s->tagrefs = (Tag **)arena_alloc(arena, (nlist ? nlist : 1) * sizeof(Tag *));
      if (!s->tagrefs) goto done;
      s->ntagrefs = 0; s->captagrefs = nlist;
      for (size_t ti = 0; ti < nlist; ti++) {
        Tag *tag = (Tag *)hmap_get(&rc.tag_map, list[ti].slug.s, list[ti].slug.n);
        if (!tag) {
          tag = (Tag *)arena_alloc(arena, sizeof(Tag));
          if (!tag) goto done;
          memset(tag, 0, sizeof *tag);
          tag->name = list[ti].name;
          tag->slug = list[ti].slug;
          Buf u = {0};
          if (!buf_putn(&u, rc.base.s, rc.base.n) || !buf_puts(&u, "/tags/") ||
              !buf_putn(&u, tag->slug.s, tag->slug.n) || !buf_putc(&u, '/')) { free(u.p); goto done; }
          char *uu = arena_strndup(arena, u.p ? u.p : "", u.len);
          size_t uun = u.len;
          free(u.p);
          if (!uu) goto done;
          tag->url.s = uu; tag->url.n = uun;
          if (!hmap_put(&rc.tag_map, tag->slug.s, tag->slug.n, tag)) goto done;
        }
        if (tag->nposts == tag->capposts) {
          size_t nc = tag->capposts ? tag->capposts * 2 : 8;
          Source **na = (Source **)arena_alloc(arena, nc * sizeof(Source *));
          if (!na) goto done;
          memcpy(na, tag->posts, tag->nposts * sizeof(Source *));
          tag->posts = na; tag->capposts = nc;
        }
        tag->posts[tag->nposts++] = s;
        s->tagrefs[s->ntagrefs++] = tag;
      }
    }
    for (size_t si = 0; si < rc.nsources; si++) {
      Source *s = rc.sources[si];
      if (s->post) continue;
      Val *tags_v = obj_gets(s->data, "tags");
      size_t ntags_in = (tags_v && tags_v->t == V_ARR) ? tags_v->nitems : 0;
      TagName *list = (TagName *)arena_alloc(arena, (ntags_in ? ntags_in : 1) * sizeof(TagName));
      if (!list) goto done;
      size_t nlist = 0;
      for (size_t ti = 0; ti < ntags_in; ti++) {
        Val *tv = tags_v->items[ti];
        size_t slen = 0;
        char *slug = slugify_c(arena, &rc.slug_cache, tv->str.s, tv->str.n, &slen);
        if (!slug) goto done;
        bool dup = false;
        for (size_t q = 0; q < nlist; q++) if (list[q].slug.n == slen && memcmp(list[q].slug.s, slug, slen) == 0) { dup = true; break; }
        if (dup) continue;
        list[nlist].name = tv->str;
        list[nlist].slug.s = slug; list[nlist].slug.n = slen;
        nlist++;
      }
      s->tagrefs = (Tag **)arena_alloc(arena, (nlist ? nlist : 1) * sizeof(Tag *));
      if (!s->tagrefs) goto done;
      s->ntagrefs = 0; s->captagrefs = nlist;
      for (size_t ti = 0; ti < nlist; ti++) {
        Tag *tag = (Tag *)hmap_get(&rc.tag_map, list[ti].slug.s, list[ti].slug.n);
        if (!tag) {
          tag = (Tag *)arena_alloc(arena, sizeof(Tag));
          if (!tag) goto done;
          memset(tag, 0, sizeof *tag);
          tag->name = list[ti].name;
          tag->slug = list[ti].slug;
          tag->url.s = ""; tag->url.n = 0;
        }
        s->tagrefs[s->ntagrefs++] = tag;
      }
    }
  }
  Tag **tags = NULL;
  size_t ntags = 0;
  {
    tags = (Tag **)arena_alloc(arena, (rc.tag_map.n ? rc.tag_map.n : 1) * sizeof(Tag *));
    if (!tags) goto done;
    for (size_t i = 0; i < rc.tag_map.cap; i++)
      if (rc.tag_map.ents[i].used) tags[ntags++] = (Tag *)rc.tag_map.ents[i].v;
    qsort(tags, ntags, sizeof(Tag *), cmp_tag_qsort);
  }
  if (prof2) { _ph[2] += (double)clock() * 1000.0 / CLOCKS_PER_SEC - _t0; _t0 = (double)clock() * 1000.0 / CLOCKS_PER_SEC; }
  // 7. Layouts.
  answer = val_new(arena, V_OBJ);
  if (!answer) goto done;
  Val *site_v = val_new(arena, V_OBJ);
  if (!site_v) goto done;
  Val *site_title = obj_gets(site, "title");
  Val *site_base = val_str(arena, rc.base.s, rc.base.n);
  if (!site_title || !site_base) goto done;
  if (!obj_sets(arena, site_v, "title", site_title) || !obj_sets(arena, site_v, "base_url", site_base)) goto done;
  Val *all_tags = val_new(arena, V_ARR);
  if (!all_tags) goto done;
  for (size_t i = 0; i < ntags; i++) {
    Tag *t = tags[i];
    Val *o = val_new(arena, V_OBJ);
    Val *nm = val_str(arena, t->name.s, t->name.n);
    Val *sl = val_str(arena, t->slug.s, t->slug.n);
    Val *uu = val_str(arena, t->url.s, t->url.n);
    Val *ct = val_num(arena, (double)t->nposts);
    if (!o || !nm || !sl || !uu || !ct) goto done;
    if (!obj_sets(arena, o, "name", nm) || !obj_sets(arena, o, "slug", sl) ||
        !obj_sets(arena, o, "url", uu) || !obj_sets(arena, o, "count", ct) ||
        !arr_push(arena, all_tags, o)) goto done;
  }
  Val *recent_n = obj_gets(site, "recent");
  long recent_k = recent_n && recent_n->t == V_NUM ? (long)recent_n->d : 0;
  if (recent_k < 0) recent_k = 0;
  if (recent_k > (long)nposts) recent_k = (long)nposts;
  Val *recent = val_new(arena, V_ARR);
  if (!recent) goto done;
  for (long k = 0; k < recent_k; k++) {
    Val *card = card_of(&rc, posts[k]);
    if (!card || !arr_push(arena, recent, card)) goto done;
  }
  // Fill the invariant cache in dependency order.
  {
    Val *V0 = val_new(arena, V_OBJ);
    if (!V0) goto done;
    if (!obj_sets(arena, V0, "site", site_v) || !obj_sets(arena, V0, "tags", all_tags) ||
        !obj_sets(arena, V0, "recent", recent)) goto done;
    StrSet seen = {0}, order = {0};
    for (size_t i = 0; i < rc.tpl.n; i++) inv_visit(&rc, rc.tpl.names[i], strlen(rc.tpl.names[i]), &seen, &order);
    free(seen.items);
    for (size_t i = 0; i < order.n; i++) {
      Buf ob = {0};
      int rr = render_tpl(&rc, order.items[i].s, order.items[i].n, V0, &ob);
      if (rr != 1) { free(ob.p); free(order.items); goto done; }
      char *str = arena_strndup(arena, ob.p ? ob.p : "", ob.len);
      size_t sl = ob.len;
      free(ob.p);
      if (!str) { free(order.items); goto done; }
      Str *ent = (Str *)arena_alloc(arena, sizeof(Str));
      if (!ent) { free(order.items); goto done; }
      ent->s = str; ent->n = sl;
      if (!hmap_put(&rc.tpl.inv_cache, order.items[i].s, order.items[i].n, ent)) { free(order.items); goto done; }
    }
    free(order.items);
  }
  Val *V = val_new(arena, V_OBJ);
  if (!V) goto done;
  if (!obj_sets(arena, V, "site", site_v) || !obj_sets(arena, V, "tags", all_tags) ||
      !obj_sets(arena, V, "recent", recent)) goto done;
  // source pages
  for (size_t i = 0; i < rc.nsources; i++) {
    Source *s = rc.sources[i];
    Val *page = page_of(&rc, s);
    if (!page) goto done;
    v_set_or_del(&rc, V, "page", page);
    v_set_or_del(&rc, V, "posts", NULL);
    v_set_or_del(&rc, V, "pager", NULL);
    v_set_or_del(&rc, V, "tag", NULL);
    Buf ob = {0};
    int rr = render_tpl(&rc, s->layout.s, s->layout.n, V, &ob);
    if (rr != 1) { free(ob.p); goto done; }
    Buf kb = {0};
    if (!buf_putn(&kb, s->out_dir.s, s->out_dir.n) || !buf_puts(&kb, "/index.html")) { free(ob.p); free(kb.p); goto done; }
    Val *html = val_str(arena, ob.p ? ob.p : "", ob.len);
    free(ob.p);
    if (!html) { free(kb.p); goto done; }
    if (!obj_set(arena, answer, kb.p, kb.len, html)) { free(kb.p); goto done; }
    free(kb.p);
  }
  // index pages
  {
    Val *per_v = obj_gets(site, "per_page");
    double per = per_v && per_v->t == V_NUM ? per_v->d : 0;
    if (!(per >= 1)) goto done; // JS would loop forever; treat as failure
    long pages = (long)((nposts + (long)per - 1) / (long)per);
    if (pages < 1) pages = 1;
    for (long k = 1; k <= pages; k++) {
      Val *plist = val_new(arena, V_ARR);
      if (!plist) goto done;
      long from = (k - 1) * (long)per;
      long to = k * (long)per;
      if (to > (long)nposts) to = (long)nposts;
      for (long j = from; j < to; j++) {
        Val *card = card_of(&rc, posts[j]);
        if (!card || !arr_push(arena, plist, card)) goto done;
      }
      Val *pager = val_new(arena, V_OBJ);
      if (!pager) goto done;
      // indexUrl(k)
      Buf ub = {0};
      if (!buf_putn(&ub, rc.base.s, rc.base.n) || !buf_putc(&ub, '/')) { free(ub.p); goto done; }
      if (k != 1) {
        char tmp[32];
        int w = snprintf(tmp, sizeof tmp, "page/%ld/", k);
        if (!buf_putn(&ub, tmp, (size_t)w)) { free(ub.p); goto done; }
      }
      char *uurl = arena_strndup(arena, ub.p ? ub.p : "", ub.len);
      size_t uurln = ub.len;
      free(ub.p);
      if (!uurl) goto done;
      // newer_url / older_url
      char *nurl = NULL, *ourl = NULL;
      size_t nurln = 0, ourn = 0;
      if (k != 1) {
        Buf nb = {0};
        if (!buf_putn(&nb, rc.base.s, rc.base.n) || !buf_putc(&nb, '/')) { free(nb.p); goto done; }
        if (k - 1 != 1) {
          char tmp[32];
          int w = snprintf(tmp, sizeof tmp, "page/%ld/", k - 1);
          if (!buf_putn(&nb, tmp, (size_t)w)) { free(nb.p); goto done; }
        }
        nurl = arena_strndup(arena, nb.p ? nb.p : "", nb.len);
        nurln = nb.len;
        free(nb.p);
        if (!nurl) goto done;
      } else { nurl = ""; nurln = 0; }
      if (k != pages) {
        Buf ob2 = {0};
        if (!buf_putn(&ob2, rc.base.s, rc.base.n) || !buf_putc(&ob2, '/')) { free(ob2.p); goto done; }
        char tmp[32];
        int w = snprintf(tmp, sizeof tmp, "page/%ld/", k + 1);
        if (!buf_putn(&ob2, tmp, (size_t)w)) { free(ob2.p); goto done; }
        ourl = arena_strndup(arena, ob2.p ? ob2.p : "", ob2.len);
        ourn = ob2.len;
        free(ob2.p);
        if (!ourl) goto done;
      } else { ourl = ""; ourn = 0; }
      Val *num_k = val_num(arena, (double)k);
      Val *num_t = val_num(arena, (double)pages);
      Val *uv = val_str(arena, uurl, uurln);
      Val *nv = val_str(arena, nurl, nurln);
      Val *ov = val_str(arena, ourl, ourn);
      if (!num_k || !num_t || !uv || !nv || !ov) goto done;
      if (!obj_sets(arena, pager, "number", num_k) || !obj_sets(arena, pager, "total", num_t) ||
          !obj_sets(arena, pager, "url", uv) || !obj_sets(arena, pager, "newer_url", nv) ||
          !obj_sets(arena, pager, "older_url", ov)) goto done;
      v_set_or_del(&rc, V, "page", NULL);
      v_set_or_del(&rc, V, "posts", plist);
      v_set_or_del(&rc, V, "pager", pager);
      v_set_or_del(&rc, V, "tag", NULL);
      Buf ob = {0};
      int rr = render_tpl(&rc, "index", 5, V, &ob);
      if (rr == -1) { free(ob.p); goto done; }
      if (rr == 1) {
        Val *html = val_str(arena, ob.p ? ob.p : "", ob.len);
        free(ob.p);
        if (!html) goto done;
        char keybuf[64];
        if (k == 1) snprintf(keybuf, sizeof keybuf, "index.html");
        else snprintf(keybuf, sizeof keybuf, "page/%ld/index.html", k);
        if (!obj_set(arena, answer, keybuf, strlen(keybuf), html)) goto done;
      } else free(ob.p);
    }
  }
  // tag pages
  for (size_t i = 0; i < ntags; i++) {
    Val *tv = tag_of(&rc, tags[i]);
    if (!tv) goto done;
    v_set_or_del(&rc, V, "page", NULL);
    v_set_or_del(&rc, V, "posts", NULL);
    v_set_or_del(&rc, V, "pager", NULL);
    v_set_or_del(&rc, V, "tag", tv);
    Buf ob = {0};
    int rr = render_tpl(&rc, "tag", 3, V, &ob);
    if (rr == -1) { free(ob.p); goto done; }
    if (rr == 1) {
      Val *html = val_str(arena, ob.p ? ob.p : "", ob.len);
      free(ob.p);
      if (!html) goto done;
      Buf kb = {0};
      if (!buf_puts(&kb, "tags/") || !buf_putn(&kb, tags[i]->slug.s, tags[i]->slug.n) ||
          !buf_puts(&kb, "/index.html")) { free(kb.p); goto done; }
      if (!obj_set(arena, answer, kb.p, kb.len, html)) { free(kb.p); goto done; }
      free(kb.p);
    } else free(ob.p);
  }
  // tags index
  {
    v_set_or_del(&rc, V, "page", NULL);
    v_set_or_del(&rc, V, "posts", NULL);
    v_set_or_del(&rc, V, "pager", NULL);
    v_set_or_del(&rc, V, "tag", NULL);
    Buf ob = {0};
    int rr = render_tpl(&rc, "tags", 4, V, &ob);
    if (rr == -1) { free(ob.p); goto done; }
    if (rr == 1) {
      Val *html = val_str(arena, ob.p ? ob.p : "", ob.len);
      free(ob.p);
      if (!html) goto done;
      if (!obj_set(arena, answer, "tags/index.html", 15, html)) goto done;
    } else free(ob.p);
  }
  // statics
  for (size_t i = 0; i < nstatics; i++) {
    if (obj_has(answer, statics[i].path.s, statics[i].path.n)) {
      char tmp[1024];
      snprintf(tmp, sizeof tmp, "shadowed %.*s: a built page has its path",
               (int)statics[i].path.n, statics[i].path.s);
      if (!report_push(&rc, tmp, strlen(tmp))) goto done;
    } else {
      Val *cv = val_str(arena, statics[i].content.s, statics[i].content.n);
      if (!cv) goto done;
      if (!obj_set(arena, answer, statics[i].path.s, statics[i].path.n, cv)) goto done;
    }
  }
  // 8. Report.
  {
    size_t total_lines = rc.nreport + rc.nbroken;
    Str *lines = (Str *)arena_alloc(arena, (total_lines ? total_lines : 1) * sizeof(Str));
    if (!lines) goto done;
    memcpy(lines, rc.report, rc.nreport * sizeof(Str));
    memcpy(lines + rc.nreport, rc.broken, rc.nbroken * sizeof(Str));
    qsort(lines, total_lines, sizeof(Str), cmp_str_qsort);
    size_t npages = 0;
    for (size_t i = 0; i < rc.nsources; i++) if (!rc.sources[i]->post) npages++;
    char built[256];
    snprintf(built, sizeof built, "built %zu pages, %zu posts, %zu tags and %zu static files",
             npages, nposts, ntags, nstatics);
    Buf b = {0};
    for (size_t i = 0; i < total_lines; i++) {
      if (!buf_putn(&b, lines[i].s, lines[i].n) || !buf_putc(&b, '\n')) { free(b.p); goto done; }
    }
    if (!buf_puts(&b, built) || !buf_putc(&b, '\n')) { free(b.p); goto done; }
    Val *rep = val_str(arena, b.p ? b.p : "", b.len);
    free(b.p);
    if (!rep) goto done;
    if (!obj_set(arena, answer, "build-report.txt", 16, rep)) goto done;
  }
  *out_answer = answer;
  ok = true;
  if (prof2) { _ph[3] += (double)clock() * 1000.0 / CLOCKS_PER_SEC - _t0;
    fprintf(stderr, "[v4-phases] tpl+fm+links %.1f | markdown %.1f | posts+tags %.1f | layouts+report %.1f ms\n",
            _ph[0], _ph[1], _ph[2], _ph[3]); }
done:
  hmap_free(&rc.slug_cache); hmap_free(&rc.link_cache); hmap_free(&rc.by_source);
  hmap_free(&rc.taken); hmap_free(&rc.static_set); hmap_free(&rc.tag_map);
  hmap_free(&rc.tpl.inv_memo); hmap_free(&rc.tpl.inv_set); hmap_free(&rc.tpl.inv_cache);
  free(rc.tpl.names); free(rc.tpl.trees);
  return ok;
}

// ---- runAll: read cases.json, run every case in C, write answers.json ----
static bool read_whole_file(const char *path, char **out, size_t *out_n) {
  FILE *f = fopen(path, "rb");
  if (!f) return false;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return false; }
  long sz = ftell(f);
  if (sz < 0) { fclose(f); return false; }
  if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return false; }
  char *buf = (char *)malloc((size_t)sz + 1);
  if (!buf) { fclose(f); return false; }
  size_t got = fread(buf, 1, (size_t)sz, f);
  fclose(f);
  if (got != (size_t)sz) { free(buf); return false; }
  buf[sz] = 0;
  *out = buf; *out_n = (size_t)sz;
  return true;
}
static bool write_whole_file(const char *path, const char *data, size_t n) {
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  size_t put = fwrite(data, 1, n, f);
  bool ok = (put == n) && (fclose(f) == 0);
  if (!ok) fclose(f);
  return ok;
}
static napi_value runall_native(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value result = NULL;
  if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) return NULL;
  if (argc < 2) { napi_throw_error(env, NULL, "runAll(inputPath, outputPath)"); return NULL; }
  bool ok = false;
  char *in_path = NULL, *out_path = NULL;
  char *raw = NULL;
  size_t raw_n = 0;
  Arena input_arena, case_arena;
  memset(&input_arena, 0, sizeof input_arena);
  memset(&case_arena, 0, sizeof case_arena);
  Buf answers = {0};
  size_t plen = 0;
  if (napi_get_value_string_utf8(env, argv[0], NULL, 0, &plen) == napi_ok) {
    in_path = (char *)malloc(plen + 1);
    if (in_path) {
      size_t g = 0;
      napi_get_value_string_utf8(env, argv[0], in_path, plen + 1, &g);
    }
  }
  plen = 0;
  if (in_path && napi_get_value_string_utf8(env, argv[1], NULL, 0, &plen) == napi_ok) {
    out_path = (char *)malloc(plen + 1);
    if (out_path) {
      size_t g = 0;
      napi_get_value_string_utf8(env, argv[1], out_path, plen + 1, &g);
    }
  }
  if (!in_path || !out_path) goto finish;
  if (!read_whole_file(in_path, &raw, &raw_n)) goto finish;
  bool prof = getenv("V4_PROFILE") != NULL;
  double t_start = (double)clock() * 1000.0 / CLOCKS_PER_SEC;
  double t_parse = 0, t_run = 0, t_ser = 0, t_write = 0;
  arena_init(&input_arena);
  arena_init(&case_arena);
  {
    JParser jp = { raw, raw + raw_n, &input_arena, false };
    Val *top = jp_value(&jp);
    jp_ws(&jp);
    if (!top || jp.err || jp.p != jp.end || top->t != V_ARR) goto finish;
    t_parse = (double)clock() * 1000.0 / CLOCKS_PER_SEC - t_start;
    if (!buf_putc(&answers, '[')) goto finish;
    for (size_t ci = 0; ci < top->nitems; ci++) {
      Val *c = top->items[ci];
      Val *inp = obj_gets(c, "input");
      if (!inp) goto finish;
      Val *ans = NULL;
      double tr0 = (double)clock() * 1000.0 / CLOCKS_PER_SEC;
      if (!run_case(&case_arena, inp, &ans)) goto finish;
      t_run += (double)clock() * 1000.0 / CLOCKS_PER_SEC - tr0;
      if (ci && !buf_putc(&answers, ',')) goto finish;
      double ts0 = (double)clock() * 1000.0 / CLOCKS_PER_SEC;
      if (!json_write_val(&answers, ans)) goto finish;
      t_ser += (double)clock() * 1000.0 / CLOCKS_PER_SEC - ts0;
      arena_reset(&case_arena);
    }
    if (!buf_putc(&answers, ']')) goto finish;
    double tw0 = (double)clock() * 1000.0 / CLOCKS_PER_SEC;
    if (!write_whole_file(out_path, answers.p ? answers.p : "", answers.len)) goto finish;
    t_write = (double)clock() * 1000.0 / CLOCKS_PER_SEC - tw0;
    ok = true;
  }
  if (prof) fprintf(stderr, "[v4-profile] parse %.1f ms | run %.1f ms | serialize %.1f ms | write %.1f ms | answers %.1f MB\n",
                   t_parse, t_run, t_ser, t_write, (double)answers.len / 1048576.0);
finish:
  free(in_path); free(out_path); free(raw);
  free(answers.p);
  arena_free(&input_arena);
  arena_free(&case_arena);
  if (napi_get_boolean(env, ok, &result) != napi_ok) return NULL;
  return result;
}
