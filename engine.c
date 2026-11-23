// engine.c — native markdown kernel for the make.fun site-generator entry.
// Byte-exact port of renderMarkdown/inline/escapeHtml from solve.js.
// Input/output are UTF-8 (from napi_get_value_string_utf8); all structural
// characters are ASCII, so byte offsets at ASCII boundaries equal JS
// code-unit offsets. JS side only calls this when the source holds no lone
// surrogates (which napi would map to U+FFFD), so the round-trip is exact.
//
// Build: gcc -O3 -fPIC -shared -I/usr/include/node -DNAPI_VERSION=8 -o engine.node engine.c
#define NAPI_VERSION 8
#include <node_api.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct { char *p; size_t len, cap; } Buf;

static bool buf_reserve(Buf *b, size_t extra) {
  if (b->len + extra <= b->cap) return true;
  size_t nc = b->cap ? b->cap : 256;
  while (nc < b->len + extra) nc *= 2;
  char *np = (char *)realloc(b->p, nc);
  if (!np) return false;
  b->p = np; b->cap = nc;
  return true;
}
static bool buf_putn(Buf *b, const char *s, size_t n) {
  if (!buf_reserve(b, n)) return false;
  memcpy(b->p + b->len, s, n); b->len += n;
  return true;
}
static bool buf_putc(Buf *b, char c) { return buf_putn(b, &c, 1); }
static bool buf_puts(Buf *b, const char *s) { return buf_putn(b, s, strlen(s)); }

// escapeHtml over bytes [s, s+n): only & < > " ' are special.
static bool buf_escape(Buf *b, const char *s, size_t n) {
  size_t i = 0;
  while (i < n) {
    size_t j = i;
    while (j < n && s[j] != '&' && s[j] != '<' && s[j] != '>' && s[j] != '"' && s[j] != '\'') j++;
    if (j > i && !buf_putn(b, s + i, j - i)) return false;
    if (j >= n) break;
    switch (s[j]) {
      case '&': if (!buf_puts(b, "&amp;")) return false; break;
      case '<': if (!buf_puts(b, "&lt;")) return false; break;
      case '>': if (!buf_puts(b, "&gt;")) return false; break;
      case '"': if (!buf_puts(b, "&quot;")) return false; break;
      case '\'': if (!buf_puts(b, "&#39;")) return false; break;
    }
    i = j + 1;
  }
  return true;
}

static bool is_space_tab(char c) { return c == ' ' || c == '\t'; }

// JS \s test on a UTF-8 range: fast ASCII path, full decode otherwise.
static bool has_js_ws(const char *s, size_t n) {
  size_t i = 0;
  while (i < n) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) {
      if (c == 0x09 || c == 0x0A || c == 0x0B || c == 0x0C || c == 0x0D || c == 0x20) return true;
      i++;
      continue;
    }
    uint32_t cp;
    int adv;
    if ((c & 0xE0) == 0xC0 && i + 1 < n) { cp = ((c & 0x1F) << 6) | ((unsigned char)s[i+1] & 0x3F); adv = 2; }
    else if ((c & 0xF0) == 0xE0 && i + 2 < n) { cp = ((c & 0x0F) << 12) | (((unsigned char)s[i+1] & 0x3F) << 6) | ((unsigned char)s[i+2] & 0x3F); adv = 3; }
    else if ((c & 0xF8) == 0xF0 && i + 3 < n) { cp = ((c & 0x07) << 18) | (((unsigned char)s[i+1] & 0x3F) << 12) | (((unsigned char)s[i+2] & 0x3F) << 6) | ((unsigned char)s[i+3] & 0x3F); adv = 4; }
    else { i++; continue; }
    if (cp == 0xA0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 ||
        cp == 0x202F || cp == 0x205F || cp == 0x3000 || cp == 0xFEFF) return true;
    i += adv;
  }
  return false;
}

static bool is_escapable(char c) {
  switch (c) {
    case '\\': case '`': case '*': case '_': case '[': case ']': case '(': case ')':
    case '#': case '+': case '-': case '.': case '!': case '>': return true;
    default: return false;
  }
}

typedef struct {
  napi_env env;
  napi_value linkFn, slugFn, global;
  char **ids; size_t nids, capids; // dedup of heading ids within one document
  bool failed;
} Ctx;

// Call a JS (string)->string function; returns malloc'd UTF-8 result or NULL (sets failed).
static char *call_str_fn(Ctx *cx, napi_value fn, const char *s, size_t n, size_t *out_len) {
  napi_value arg, ret;
  if (napi_create_string_utf8(cx->env, s, n, &arg) != napi_ok) { cx->failed = true; return NULL; }
  if (napi_call_function(cx->env, cx->global, fn, 1, &arg, &ret) != napi_ok) { cx->failed = true; return NULL; }
  size_t len = 0;
  if (napi_get_value_string_utf8(cx->env, ret, NULL, 0, &len) != napi_ok) { cx->failed = true; return NULL; }
  char *out = (char *)malloc(len + 1);
  if (!out) { cx->failed = true; return NULL; }
  size_t got = 0;
  if (napi_get_value_string_utf8(cx->env, ret, out, len + 1, &got) != napi_ok) { free(out); cx->failed = true; return NULL; }
  out[len] = 0;
  if (out_len) *out_len = len;
  return out;
}

// findUnescaped(s, what, from): -1 if none.
static long find_unescaped(const char *s, size_t n, const char *what, size_t wlen, size_t from) {
  size_t j = from;
  while (j + wlen <= n) {
    if (s[j] == '\\' && j + 1 < n && is_escapable(s[j+1])) { j += 2; continue; }
    if (memcmp(s + j, what, wlen) == 0) return (long)j;
    j++;
  }
  return -1;
}
static long find_single_star(const char *s, size_t n, size_t from) {
  for (size_t j = from; j < n; j++) {
    char c = s[j];
    if (c == '\\' && j + 1 < n && is_escapable(s[j+1])) { j++; continue; }
    if (c != '*') continue;
    if (j + 1 < n && s[j+1] == '*') { j++; continue; }
    return (long)j;
  }
  return -1;
}
static long find_char(const char *s, size_t n, char c, size_t from) {
  for (size_t j = from; j < n; j++) if (s[j] == c) return (long)j;
  return -1;
}

static bool inline_md(Ctx *cx, const char *s, size_t n, Buf *out);

static bool inline_md(Ctx *cx, const char *s, size_t n, Buf *out) {
  size_t i = 0;
  while (i < n) {
    // next special among \ ` * [
    size_t next = n;
    for (size_t j = i; j < n; j++) {
      char c = s[j];
      if (c == '\\' || c == '`' || c == '*' || c == '[') { next = j; break; }
    }
    if (next > i) { if (!buf_escape(out, s + i, next - i)) return false; i = next; continue; }
    if (i >= n) break;
    char c = s[i];
    if (c == '\\' && i + 1 < n && is_escapable(s[i+1])) {
      if (!buf_escape(out, s + i + 1, 1)) return false;
      i += 2; continue;
    }
    if (c == '`') {
      long j = find_char(s, n, '`', i + 1);
      if (j > (long)i + 1) {
        if (!buf_puts(out, "<code>")) return false;
        if (!buf_escape(out, s + i + 1, (size_t)j - i - 1)) return false;
        if (!buf_puts(out, "</code>")) return false;
        i = (size_t)j + 1;
      } else { if (!buf_putc(out, '`')) return false; i++; }
      continue;
    }
    if (c == '*') {
      if (i + 1 < n && s[i+1] == '*') {
        long j = find_unescaped(s, n, "**", 2, i + 2);
        if (j > (long)i + 2) {
          if (!buf_puts(out, "<strong>")) return false;
          if (!inline_md(cx, s + i + 2, (size_t)j - i - 2, out)) return false;
          if (!buf_puts(out, "</strong>")) return false;
          i = (size_t)j + 2;
        } else { if (!buf_puts(out, "**")) return false; i += 2; }
        continue;
      }
      long j = find_single_star(s, n, i + 1);
      if (j > (long)i + 1) {
        if (!buf_puts(out, "<em>")) return false;
        if (!inline_md(cx, s + i + 1, (size_t)j - i - 1, out)) return false;
        if (!buf_puts(out, "</em>")) return false;
        i = (size_t)j + 1;
      } else { if (!buf_putc(out, '*')) return false; i++; }
      continue;
    }
    if (c == '[') {
      long k = find_unescaped(s, n, "]", 1, i + 1);
      long m = -1;
      if (k != -1 && (size_t)k + 1 < n && s[k+1] == '(') m = find_char(s, n, ')', (size_t)k + 2);
      size_t tlen = 0;
      const char *tgt = "";
      if (m != -1) { tgt = s + k + 2; tlen = (size_t)m - (size_t)k - 2; }
      if (tlen > 0 && !has_js_ws(tgt, tlen)) {
        size_t href_len = 0;
        char *href = call_str_fn(cx, cx->linkFn, tgt, tlen, &href_len);
        if (cx->failed) return false;
        if (!buf_puts(out, "<a href=\"")) { free(href); return false; }
        if (!buf_escape(out, href, href_len)) { free(href); return false; }
        free(href);
        if (!buf_puts(out, "\">")) return false;
        if (!inline_md(cx, s + i + 1, (size_t)k - i - 1, out)) return false;
        if (!buf_puts(out, "</a>")) return false;
        i = (size_t)m + 1;
        continue;
      }
      if (!buf_putc(out, '[')) return false;
      i++;
      continue;
    }
    if (!buf_escape(out, s + i, 1)) return false;
    i++;
  }
  return true;
}

// ---- line helpers (lines are byte ranges, no copying) ----
typedef struct { const char *p; size_t n; } Line;

static bool line_blank(const Line *l) {
  for (size_t i = 0; i < l->n; i++) if (l->p[i] != ' ' && l->p[i] != '\t') return false;
  return true;
}
// trimLine into a Buf (returns the trimmed range via out pointers)
static void trim_range(const Line *l, const char **a, size_t *n) {
  size_t s = 0, e = l->n;
  while (s < e && is_space_tab(l->p[s])) s++;
  while (e > s && is_space_tab(l->p[e-1])) e--;
  *a = l->p + s; *n = e - s;
}
// heading: depth + text range, or 0.
static int heading_of(const Line *l, const char **text, size_t *tlen) {
  size_t n = 0;
  while (n < l->n && l->p[n] == '#') n++;
  if (n == 0 || n > 6) return 0;
  if (n >= l->n) return 0;
  char c = l->p[n];
  if (c != ' ' && c != '\t') return 0;
  const char *t = l->p + n + 1;
  size_t tl = l->n - n - 1;
  while (tl > 0 && is_space_tab(t[tl-1])) tl--;
  if (tl == 0) return 0;
  *text = t; *tlen = tl;
  return (int)n;
}
// fence open: returns 1 and lang range (may be empty); 0 if not a fence.
static bool fence_open(const Line *l, const char **lang, size_t *llen) {
  if (l->n < 3 || memcmp(l->p, "```", 3) != 0) return false;
  size_t i = 3;
  while (i < l->n) {
    char c = l->p[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '+' || c == '-') i++;
    else break;
  }
  *lang = l->p + 3; *llen = i - 3;
  while (i < l->n && is_space_tab(l->p[i])) i++;
  return i == l->n;
}
static bool fence_close(const Line *l) {
  if (l->n < 3 || memcmp(l->p, "```", 3) != 0) return false;
  for (size_t i = 3; i < l->n; i++) if (!is_space_tab(l->p[i])) return false;
  return true;
}
static bool is_hr(const Line *l) {
  if (l->n == 3 && (memcmp(l->p, "---", 3) == 0 || memcmp(l->p, "***", 3) == 0)) return true;
  if (l->n >= 3 && (memcmp(l->p, "---", 3) == 0 || memcmp(l->p, "***", 3) == 0)) {
    for (size_t i = 3; i < l->n; i++) if (!is_space_tab(l->p[i])) return false;
    return true;
  }
  return false;
}
static bool ends_paragraph(Ctx *cx, const Line *l) {
  (void)cx;
  if (line_blank(l)) return true;
  const char *x; size_t xn;
  if (fence_open(l, &x, &xn)) return true;
  const char *t; size_t tl;
  if (heading_of(l, &t, &tl)) return true;
  return is_hr(l);
}

// uniqueSlug within this document (ids list holds malloc'd copies).
static char *unique_id(Ctx *cx, const char *slug, size_t slen, size_t *out_len) {
  // try slug, slug-2, slug-3, ...
  for (int n = 1; ; n++) {
    Buf b = {0};
    if (!buf_putn(&b, slug, slen)) { free(b.p); return NULL; }
    if (n > 1) {
      char num[24];
      int w = snprintf(num, sizeof num, "-%d", n);
      if (!buf_putn(&b, num, (size_t)w)) { free(b.p); return NULL; }
    }
    bool taken = false;
    for (size_t k = 0; k < cx->nids; k++) {
      if (strlen(cx->ids[k]) == b.len && memcmp(cx->ids[k], b.p, b.len) == 0) { taken = true; break; }
    }
    if (!taken) {
      if (cx->nids == cx->capids) {
        size_t nc = cx->capids ? cx->capids * 2 : 16;
        char **ni = (char **)realloc(cx->ids, nc * sizeof *ni);
        if (!ni) { free(b.p); return NULL; }
        cx->ids = ni; cx->capids = nc;
      }
      char *copy = (char *)malloc(b.len + 1);
      if (!copy) { free(b.p); return NULL; }
      memcpy(copy, b.p, b.len); copy[b.len] = 0;
      cx->ids[cx->nids++] = copy;
      *out_len = b.len;
      return b.p; // caller frees
    }
    free(b.p);
  }
}

typedef struct { char *html; size_t hlen; Buf toc_json; bool has_excerpt; Buf excerpt; } MdOut;

// Build the result object {html, toc, excerpt} directly via N-API.
static napi_value md_native(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  if (napi_get_cb_info(env, info, &argc, argv, NULL, NULL) != napi_ok) return NULL;
  if (argc < 3) { napi_throw_error(env, NULL, "md(src, linkFn, slugFn)"); return NULL; }
  size_t srclen = 0;
  if (napi_get_value_string_utf8(env, argv[0], NULL, 0, &srclen) != napi_ok) return NULL;
  char *src = (char *)malloc(srclen + 1);
  if (!src) { napi_throw_error(env, NULL, "oom"); return NULL; }
  size_t got = 0;
  if (napi_get_value_string_utf8(env, argv[0], src, srclen + 1, &got) != napi_ok) { free(src); return NULL; }
  src[srclen] = 0;

  Ctx cx = { .env = env, .linkFn = argv[1], .slugFn = argv[2], .ids = NULL, .nids = 0, .capids = 0, .failed = false };
  if (napi_get_global(env, &cx.global) != napi_ok) { free(src); napi_throw_error(env, NULL, "no global"); return NULL; }

  // split into lines (JS split('\n'))
  size_t nlines = 1;
  for (size_t i = 0; i < srclen; i++) if (src[i] == '\n') nlines++;
  Line *lines = (Line *)malloc(nlines * sizeof *lines);
  if (!lines) { free(src); napi_throw_error(env, NULL, "oom"); return NULL; }
  size_t li = 0, start = 0;
  for (size_t i = 0; i <= srclen; i++) {
    if (i == srclen || src[i] == '\n') { lines[li].p = src + start; lines[li].n = i - start; li++; start = i + 1; }
  }

  Buf blocks = {0};       // joined blocks
  // toc entries: parallel arrays of level, id string, html string
  int *toc_level = NULL; char **toc_id = NULL; size_t *toc_idlen = NULL; char **toc_html = NULL; size_t *toc_htmllen = NULL;
  size_t ntoc = 0, captoc = 0;
  Buf excerpt = {0}; bool has_excerpt = false;
  bool ok = true;
  size_t nblocks = 0;

#define BLOCK_SEP() do { if (nblocks > 0) { if (!buf_putc(&blocks, '\n')) { ok = false; } } nblocks++; } while (0)

  size_t i = 0;
  size_t L = nlines;
  while (i < L && ok) {
    const Line *line = &lines[i];
    if (line_blank(line)) { i++; continue; }
    const char *lang = NULL; size_t llen = 0;
    if (fence_open(line, &lang, &llen)) {
      Buf body = {0};
      i++;
      while (i < L && !fence_close(&lines[i])) {
        if (!buf_putn(&body, lines[i].p, lines[i].n) || !buf_putc(&body, '\n')) { ok = false; break; }
        i++;
      }
      i++;
      BLOCK_SEP();
      if (!buf_puts(&blocks, "<pre><code")) ok = false;
      if (llen > 0) {
        if (!buf_puts(&blocks, " class=\"language-")) ok = false;
        if (!buf_escape(&blocks, lang, llen)) ok = false;
        if (!buf_putc(&blocks, '"')) ok = false;
      }
      if (!buf_putc(&blocks, '>')) ok = false;
      // code = escape(body) per line + '\n' == escape whole body (no specials in \n)
      if (!buf_escape(&blocks, body.p ? body.p : "", body.len)) ok = false;
      free(body.p);
      if (!buf_puts(&blocks, "</code></pre>")) ok = false;
      continue;
    }
    const char *htext = NULL; size_t htlen = 0;
    int depth = heading_of(line, &htext, &htlen);
    if (depth) {
      size_t slug_len = 0;
      char *slug = call_str_fn(&cx, cx.slugFn, htext, htlen, &slug_len);
      if (cx.failed) { ok = false; break; }
      size_t idlen = 0;
      char *id = unique_id(&cx, slug, slug_len, &idlen);
      free(slug);
      if (!id) { ok = false; break; }
      Buf hb = {0};
      if (!inline_md(&cx, htext, htlen, &hb)) { free(id); ok = false; break; }
      BLOCK_SEP();
      char hdr[64];
      int hw = snprintf(hdr, sizeof hdr, "<h%d id=\"", depth);
      if (!buf_putn(&blocks, hdr, (size_t)hw) || !buf_putn(&blocks, id, idlen)) { free(id); free(hb.p); ok = false; break; }
      hw = snprintf(hdr, sizeof hdr, "\">");
      if (!buf_putn(&blocks, hdr, (size_t)hw) || !buf_putn(&blocks, hb.p ? hb.p : "", hb.len)) { free(id); free(hb.p); ok = false; break; }
      hw = snprintf(hdr, sizeof hdr, "</h%d>", depth);
      if (!buf_putn(&blocks, hdr, (size_t)hw)) { free(id); free(hb.p); ok = false; break; }
      if (depth == 2 || depth == 3) {
        if (ntoc == captoc) {
          size_t nc = captoc ? captoc * 2 : 16;
          int *nl = (int *)realloc(toc_level, nc * sizeof *nl);
          char **ni = (char **)realloc(toc_id, nc * sizeof *ni);
          size_t *nil = (size_t *)realloc(toc_idlen, nc * sizeof *nil);
          char **nh = (char **)realloc(toc_html, nc * sizeof *nh);
          size_t *nhl = (size_t *)realloc(toc_htmllen, nc * sizeof *nhl);
          if (!nl || !ni || !nil || !nh || !nhl) { free(nl); free(ni); free(nil); free(nh); free(nhl); free(id); free(hb.p); ok = false; break; }
          toc_level = nl; toc_id = ni; toc_idlen = nil; toc_html = nh; toc_htmllen = nhl; captoc = nc;
        }
        toc_level[ntoc] = depth;
        toc_id[ntoc] = id; toc_idlen[ntoc] = idlen; // ownership transferred
        toc_html[ntoc] = hb.p; toc_htmllen[ntoc] = hb.len;
        ntoc++;
        id = NULL; hb.p = NULL;
      }
      free(id); free(hb.p);
      i++;
      continue;
    }
    if (is_hr(line)) { BLOCK_SEP(); if (!buf_puts(&blocks, "<hr>")) ok = false; i++; continue; }
    if (line->n > 0 && line->p[0] == '>') {
      // collect consecutive '>' lines, strip /^> ?/", trim, drop empties
      Buf joined = {0};
      size_t nkept = 0;
      while (i < L && lines[i].n > 0 && lines[i].p[0] == '>') {
        const Line *bl = &lines[i];
        const char *bp = bl->p + 1; size_t bn = bl->n - 1;
        if (bn > 0 && bp[0] == ' ') { bp++; bn--; }
        const char *ta = bp; size_t tn = bn;
        size_t s = 0, e = tn;
        while (s < e && is_space_tab(ta[s])) s++;
        while (e > s && is_space_tab(ta[e-1])) e--;
        if (e > s) {
          if (nkept > 0 && !buf_putc(&joined, '\n')) { ok = false; break; }
          if (!buf_putn(&joined, ta + s, e - s)) { ok = false; break; }
          nkept++;
        }
        i++;
      }
      BLOCK_SEP();
      if (nkept == 0) {
        if (!buf_puts(&blocks, "<blockquote>\n</blockquote>")) ok = false;
      } else {
        Buf ob = {0};
        if (!inline_md(&cx, joined.p, joined.len, &ob)) ok = false;
        if (!buf_puts(&blocks, "<blockquote>\n<p>")) ok = false;
        if (!buf_putn(&blocks, ob.p ? ob.p : "", ob.len)) ok = false;
        if (!buf_puts(&blocks, "</p>\n</blockquote>")) ok = false;
        free(ob.p);
      }
      free(joined.p);
      continue;
    }
    // list?
    bool isBullet = false, isOrdered = false;
    long ordStart = 1;
    const char *itemText = NULL; size_t itemLen = 0;
    if ((line->n >= 2 && line->p[0] == '-' && line->p[1] == ' ') || (line->n >= 2 && line->p[0] == '*' && line->p[1] == ' ')) {
      isBullet = true; itemText = line->p + 2; itemLen = line->n - 2;
    } else {
      size_t d = 0;
      while (d < line->n && line->p[d] >= '0' && line->p[d] <= '9') d++;
      if (d >= 1 && d <= 9 && d < line->n && line->p[d] == '.' && d + 1 < line->n && line->p[d+1] == ' ') {
        isOrdered = true;
        long v = 0; for (size_t q = 0; q < d; q++) v = v * 10 + (line->p[q] - '0');
        ordStart = v; itemText = line->p + d + 2; itemLen = line->n - d - 2;
      }
    }
    if (isBullet || isOrdered) {
      // items: array of (ptr,len) joined pieces
      typedef struct { Buf b; } Item;
      Item *items = NULL; size_t nitems = 0, capitems = 0;
      Buf cur = {0};
      {
        const char *a; size_t an;
        Line tmp = { itemText, itemLen };
        trim_range(&tmp, &a, &an);
        if (!buf_putn(&cur, a, an)) ok = false;
      }
      i++;
      while (i < L && ok) {
        const Line *ln = &lines[i];
        const char *mText = NULL; size_t mLen = 0; bool mOrd = false;
        if (isOrdered) {
          size_t d = 0;
          while (d < ln->n && ln->p[d] >= '0' && ln->p[d] <= '9') d++;
          if (d >= 1 && d <= 9 && d < ln->n && ln->p[d] == '.' && d + 1 < ln->n && ln->p[d+1] == ' ') { mOrd = true; mText = ln->p + d + 2; mLen = ln->n - d - 2; }
        } else if (ln->n >= 2 && ((ln->p[0] == '-' && ln->p[1] == ' ') || (ln->p[0] == '*' && ln->p[1] == ' '))) {
          mText = ln->p + 2; mLen = ln->n - 2;
        }
        (void)mOrd;
        if (mText != NULL) {
          if (nitems == capitems) {
            size_t nc = capitems ? capitems * 2 : 8;
            Item *ni = (Item *)realloc(items, nc * sizeof *ni);
            if (!ni) { ok = false; break; }
            items = ni; capitems = nc;
          }
          items[nitems].b = cur; nitems++;
          cur.p = NULL; cur.len = 0; cur.cap = 0;
          Line tmp = { mText, mLen };
          const char *a; size_t an; trim_range(&tmp, &a, &an);
          if (!buf_putn(&cur, a, an)) { ok = false; break; }
          i++;
        } else if (ln->n >= 3 && ln->p[0] == ' ' && ln->p[1] == ' ' && ln->p[2] != ' ' && ln->p[2] != '\t') {
          const char *ta; size_t tan; trim_range(ln, &ta, &tan);
          if (tan != 0) {
            if (!buf_putc(&cur, '\n')) { ok = false; break; }
            if (!buf_putn(&cur, ta, tan)) { ok = false; break; }
            i++;
          } else break;
        } else if (ln->n >= 2 && ln->p[0] == ' ' && ln->p[1] == ' ' && (ln->p[2] == ' ' || ln->p[2] == '\t')) {
          size_t k = 0; while (k < ln->n && ln->p[k] == ' ') k++;
          if (k >= 2 && k < ln->n && ln->p[k] != ' ' && ln->p[k] != '\t') {
            if (!buf_putc(&cur, '\n')) { ok = false; break; }
            const char *a; size_t an; trim_range(ln, &a, &an);
            if (!buf_putn(&cur, a, an)) { ok = false; break; }
            i++;
          } else break;
        } else break;
      }
      if (ok) {
        if (nitems == capitems) {
          size_t nc = capitems ? capitems * 2 : 8;
          Item *ni = (Item *)realloc(items, nc * sizeof *ni);
          if (!ni) ok = false; else { items = ni; capitems = nc; }
        }
        if (ok) { items[nitems].b = cur; nitems++; cur.p = NULL; }
      }
      if (ok) {
        BLOCK_SEP();
        if (isOrdered && ordStart != 1) {
          char ob[64]; int w = snprintf(ob, sizeof ob, "<ol start=\"%ld\">\n", ordStart);
          if (!buf_putn(&blocks, ob, (size_t)w)) ok = false;
        } else if (isOrdered) {
          if (!buf_puts(&blocks, "<ol>\n")) ok = false;
        } else {
          if (!buf_puts(&blocks, "<ul>\n")) ok = false;
        }
        for (size_t it = 0; it < nitems && ok; it++) {
          Buf ob = {0};
          if (!inline_md(&cx, items[it].b.p ? items[it].b.p : "", items[it].b.len, &ob)) { ok = false; break; }
          if (!buf_puts(&blocks, "<li>")) ok = false;
          if (!buf_putn(&blocks, ob.p ? ob.p : "", ob.len)) ok = false;
          if (!buf_puts(&blocks, "</li>\n")) ok = false;
          free(ob.p);
        }
        if (ok) { if (!buf_puts(&blocks, isOrdered ? "</ol>" : "</ul>")) ok = false; }
      }
      for (size_t q = 0; q < nitems; q++) free(items[q].b.p);
      free(items); free(cur.p);
      continue;
    }
    // paragraph
    {
      Buf body = {0};
      {
        const char *a; size_t an; trim_range(line, &a, &an);
        if (!buf_putn(&body, a, an)) ok = false;
      }
      i++;
      while (i < L && ok && !ends_paragraph(&cx, &lines[i])) {
        if (!buf_putc(&body, '\n')) { ok = false; break; }
        const char *a; size_t an; trim_range(&lines[i], &a, &an);
        if (!buf_putn(&body, a, an)) { ok = false; break; }
        i++;
      }
      if (ok) {
        Buf ob = {0};
        if (!inline_md(&cx, body.p ? body.p : "", body.len, &ob)) ok = false;
        if (ok) {
          BLOCK_SEP();
          if (!buf_puts(&blocks, "<p>")) ok = false;
          if (!buf_putn(&blocks, ob.p ? ob.p : "", ob.len)) ok = false;
          if (!buf_puts(&blocks, "</p>")) ok = false;
          if (!has_excerpt) {
            if (!buf_putn(&excerpt, ob.p ? ob.p : "", ob.len)) ok = false;
            has_excerpt = true;
          }
        }
        free(ob.p);
      }
      free(body.p);
    }
  }

  napi_value result = NULL;
  if (ok && !cx.failed) {
    napi_value obj, html_str, toc_arr, excerpt_str;
    if (napi_create_object(env, &obj) != napi_ok) ok = false;
    // JS returns blocks.join('\n') + '\n' when non-empty; blocks hold the
    // join, so append the trailing newline here.
    if (ok && nblocks > 0 && !buf_putc(&blocks, '\n')) ok = false;
    if (ok && napi_create_string_utf8(env, blocks.p ? blocks.p : "", blocks.len, &html_str) != napi_ok) ok = false;
    if (ok && napi_create_array_with_length(env, ntoc, &toc_arr) != napi_ok) ok = false;
    for (size_t t = 0; t < ntoc && ok; t++) {
      napi_value e, lv, idv, hv;
      if (napi_create_object(env, &e) != napi_ok) { ok = false; break; }
      if (napi_create_int32(env, toc_level[t], &lv) != napi_ok) { ok = false; break; }
      if (napi_create_string_utf8(env, toc_id[t], toc_idlen[t], &idv) != napi_ok) { ok = false; break; }
      if (napi_create_string_utf8(env, toc_html[t], toc_htmllen[t], &hv) != napi_ok) { ok = false; break; }
      if (napi_set_named_property(env, e, "level", lv) != napi_ok) { ok = false; break; }
      if (napi_set_named_property(env, e, "id", idv) != napi_ok) { ok = false; break; }
      if (napi_set_named_property(env, e, "html", hv) != napi_ok) { ok = false; break; }
      if (napi_set_element(env, toc_arr, (uint32_t)t, e) != napi_ok) { ok = false; break; }
    }
    if (ok && napi_create_string_utf8(env, excerpt.p ? excerpt.p : "", excerpt.len, &excerpt_str) != napi_ok) ok = false;
    if (ok) {
      if (napi_set_named_property(env, obj, "html", html_str) != napi_ok) ok = false;
      if (napi_set_named_property(env, obj, "toc", toc_arr) != napi_ok) ok = false;
      if (napi_set_named_property(env, obj, "excerpt", excerpt_str) != napi_ok) ok = false;
    }
    if (ok) result = obj;
  }
  (void)toc_html;
  free(blocks.p); free(excerpt.p); free(lines); free(src);
  for (size_t k = 0; k < cx.nids; k++) free(cx.ids[k]);
  free(cx.ids);
  for (size_t t = 0; t < ntoc; t++) { free(toc_id[t]); free(toc_html[t]); }
  free(toc_level); free(toc_id); free(toc_idlen); free(toc_html); free(toc_htmllen);
  if (!ok || cx.failed) {
    napi_throw_error(env, NULL, "engine.md failed");
    return NULL;
  }
  return result;
}

static napi_value init(napi_env env, napi_value exports) {
  napi_value fn;
  if (napi_create_function(env, "md", NAPI_AUTO_LENGTH, md_native, NULL, &fn) != napi_ok) return NULL;
  if (napi_set_named_property(env, exports, "md", fn) != napi_ok) return NULL;
  return exports;
}

NAPI_MODULE(NODE_GYP_MODULE_NAME, init)
