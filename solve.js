// Fast drop-in for makefun-site 1.0.0 — byte-exact, single-pass, no dependencies.
// Strategy: fused pipeline (no token/plugin allocations), templates compiled to
// JS functions (new Function) once per case, markdown rendered in one pass with
// bulk plain-text runs, slugify/escape/link results cached per case, and
// invariant include fragments (site/tags/recent only) rendered once per case.

const ESCAPES = { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' };
const MONTHS = ['January', 'February', 'March', 'April', 'May', 'June', 'July', 'August', 'September', 'October', 'November', 'December'];
const RESERVED = ['posts', 'tags', 'page'];

const ESC_RE = /[&<>"']/;
export function escapeHtml(text) {
  if (!ESC_RE.test(text)) return text;
  return text.replace(/[&<>"']/g, (c) => ESCAPES[c]);
}

function longDate(date) {
  // "2026-11-09" -> "9 November 2026", from the string alone.
  return `${Number(date.slice(8, 10))} ${MONTHS[Number(date.slice(5, 7)) - 1]} ${date.slice(0, 4)}`;
}

// ---- slugify (cached per case) ----
let slugCache = null;
function slugify(text) {
  const hit = slugCache.get(text);
  if (hit !== undefined) return hit;
  const plain = text.normalize('NFKD').replace(/\p{M}/gu, '');
  let out = '';
  let gap = false;
  for (const ch of plain) {
    let keep = null;
    if (ch >= 'A' && ch <= 'Z') keep = ch.toLowerCase();
    else if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) keep = ch;
    if (keep === null) { gap = out.length > 0; continue; }
    if (gap) out += '-';
    gap = false;
    out += keep;
  }
  out = out.slice(0, 64).replace(/-+$/, '');
  if (out === '') out = 'untitled';
  slugCache.set(text, out);
  return out;
}

function uniqueSlug(slug, taken) {
  let out = slug;
  for (let n = 2; taken.has(out); n++) out = `${slug}-${n}`;
  taken.add(out);
  return out;
}

function isWs(c) {
  return c <= 32 || c === 160 || c === 5760 || (c >= 8192 && c <= 8202) || c === 8232 || c === 8233 || c === 8239 || c === 8287 || c === 12288 || c === 65279;
}
function wordsOf(text) {
  let n = 0;
  let inWord = false;
  for (let i = 0; i < text.length; i++) {
    if (isWs(text.charCodeAt(i))) inWord = false;
    else if (!inWord) { inWord = true; n++; }
  }
  return n;
}

// ---- front matter (ported from makefun-site's lib/frontmatter.js) ----
const KEYS = ['title', 'date', 'tags', 'draft', 'layout', 'slug', 'summary'];
const LINE = /^([a-z_]+):(?: (.*))?$/;
const DATE = /^(\d{4})-(\d{2})-(\d{2})$/;
const NAME = /^[a-z0-9-]+$/;
const SLUG = /^[a-z0-9]+(?:-[a-z0-9]+)*$/;
const TAG = /^[^"[\]]+$/;

function daysIn(y, m) {
  if (m === 2) return (y % 4 === 0 && y % 100 !== 0) || y % 400 === 0 ? 29 : 28;
  return m === 4 || m === 6 || m === 9 || m === 11 ? 30 : 31;
}

function stringOf(raw) {
  if (raw.startsWith('"')) {
    if (raw.length < 2 || !raw.endsWith('"')) return { error: 'a quoted string is not closed' };
    let out = '';
    for (let i = 1; i < raw.length - 1; i++) {
      const c = raw[i];
      if (c === '\\') {
        const n = raw[i + 1];
        if ((n !== '"' && n !== '\\') || i + 1 === raw.length - 1) return { error: 'a quoted string holds an escape other than \\" or \\\\' };
        out += n;
        i++;
      } else if (c === '"') return { error: 'a quoted string holds a stray quote' };
      else out += c;
    }
    return { value: out };
  }
  if (raw.startsWith('[')) return { error: 'a bare string opens on [' };
  return { value: raw };
}

function valueOf(key, raw) {
  switch (key) {
    case 'title':
    case 'summary': {
      const s = stringOf(raw);
      return 'error' in s ? { error: `key "${key}": ${s.error}` } : s;
    }
    case 'date': {
      const m = DATE.exec(raw);
      const y = m ? Number(m[1]) : 0;
      const mo = m ? Number(m[2]) : 0;
      const d = m ? Number(m[3]) : 0;
      if (!m || y < 1970 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > daysIn(y, mo)) return { error: 'key "date": not a date from 1970-01-01 to 2099-12-31 (YYYY-MM-DD)' };
      return { value: raw };
    }
    case 'tags': {
      if (!raw.startsWith('[') || !raw.endsWith(']')) return { error: 'key "tags": not a list [a, b]' };
      const inner = raw.slice(1, -1);
      if (inner.trim() === '') return { value: [] };
      const tags = inner.split(',').map((t) => t.trim());
      if (tags.some((t) => t === '' || !TAG.test(t))) return { error: 'key "tags": an empty tag, or one holding " [ or ]' };
      return { value: tags };
    }
    case 'draft':
      return raw === 'true' ? { value: true } : raw === 'false' ? { value: false } : { error: 'key "draft": true or false' };
    case 'layout':
      return NAME.test(raw) ? { value: raw } : { error: 'key "layout": not a template name' };
    case 'slug':
      return SLUG.test(raw) && raw.length <= 64 ? { value: raw } : { error: 'key "slug": not a slug' };
    default:
      return { error: `unknown key "${key}"` };
  }
}

// Returns {data, body} or {error}. `text` already has "\n" line ends.
function parseFrontMatter(text) {
  if (!text.startsWith('---\n') && text !== '---') return { error: 'no front matter' };
  // Find the closing line "---": it must be a whole line.
  let close = -1;
  let pos = 4; // after "---\n"
  if (text === '---') return { error: 'the front matter is not closed' };
  while (true) {
    const nl = text.indexOf('\n---', pos - 1);
    if (nl === -1) break;
    const after = nl + 4;
    if (after === text.length || text[after] === '\n') { close = nl + 1; break; }
    pos = nl + 4;
  }
  if (close === -1) return { error: 'the front matter is not closed' };
  const fmText = text.slice(4, close - 1); // lines 1..close-1, without trailing \n
  const data = {};
  if (fmText !== '') {
    const lines = fmText.split('\n');
    for (let i = 0; i < lines.length; i++) {
      const line = lines[i];
      if (/^[ \t]*$/.test(line) || line.startsWith('#')) continue;
      const m = LINE.exec(line);
      if (!m) return { error: `line ${i + 2} is not "key: value"` };
      const key = m[1];
      const raw = (m[2] ?? '').replace(/[ \t]+$/, '');
      if (!KEYS.includes(key)) return { error: `unknown key "${key}"` };
      if (Object.hasOwn(data, key)) return { error: `key "${key}" is given twice` };
      if (raw === '') return { error: `key "${key}" has no value` };
      const v = valueOf(key, raw);
      if ('error' in v) return v;
      data[key] = v.value;
    }
  }
  if (!Object.hasOwn(data, 'title')) return { error: 'no title' };
  const bodyStart = close + 4; // skip "---\n"
  return { data, body: bodyStart <= text.length ? text.slice(bodyStart) : '' };
}

// ---- Markdown (single-pass, ported semantics from makefun-site's lib/markdown.js) ----
const SPECIAL = /[\\`*[\[]/g;
const ESCAPABLE = new Set(['\\', '`', '*', '_', '[', ']', '(', ')', '#', '+', '-', '.', '!', '>']);

function isBlank(line) {
  for (let i = 0; i < line.length; i++) {
    const c = line[i];
    if (c !== ' ' && c !== '\t') return false;
  }
  return true;
}
function trimLine(line) {
  let a = 0, b = line.length;
  while (a < b && (line[a] === ' ' || line[a] === '\t')) a++;
  while (b > a && (line[b - 1] === ' ' || line[b - 1] === '\t')) b--;
  return a === 0 && b === line.length ? line : line.slice(a, b);
}
// heading: 1-6 #, space/tab, non-empty text (trailing spaces/tabs dropped)
function headingOf(line) {
  let n = 0;
  while (n < line.length && line[n] === '#') n++;
  if (n === 0 || n > 6) return null;
  const c = line[n];
  if (c !== ' ' && c !== '\t') return null;
  const text = line.slice(n + 1).replace(/[ \t]+$/, '');
  if (text === '') return null;
  return { depth: n, text };
}
function isFenceOpen(line) {
  if (!line.startsWith('```')) return null;
  let i = 3;
  let lang = '';
  while (i < line.length) {
    const c = line[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c === '_' || c === '+' || c === '-') { lang += c; i++; }
    else break;
  }
  while (i < line.length && (line[i] === ' ' || line[i] === '\t')) i++;
  return i === line.length ? lang : null;
}
function isFenceClose(line) {
  if (!line.startsWith('```')) return false;
  for (let i = 3; i < line.length; i++) if (line[i] !== ' ' && line[i] !== '\t') return false;
  return true;
}
function isHr(line) {
  if (line !== '---' && line !== '***') {
    if (!line.startsWith('---') && !line.startsWith('***')) return false;
    const head = line.slice(0, 3);
    for (let i = 3; i < line.length; i++) if (line[i] !== ' ' && line[i] !== '\t') return false;
    return true;
  }
  return true;
}
function endsParagraph(line) {
  return isBlank(line) || isFenceOpen(line) !== null || headingOf(line) !== null || isHr(line);
}

// Find `what` from `from`, skipping backslash-escaped escapable chars. -1 if none.
function findUnescaped(s, what, from) {
  let j = from;
  while (j <= s.length - what.length) {
    if (s[j] === '\\' && j + 1 < s.length && ESCAPABLE.has(s[j + 1])) { j += 2; continue; }
    if (s.startsWith(what, j)) return j;
    j++;
  }
  return -1;
}
function findSingleStar(s, from) {
  for (let j = from; j < s.length; j++) {
    const c = s[j];
    if (c === '\\' && j + 1 < s.length && ESCAPABLE.has(s[j + 1])) { j++; continue; }
    if (c !== '*') continue;
    if (s[j + 1] === '*') { j++; continue; }
    return j;
  }
  return -1;
}

// Inline renderer: bulk plain runs (4x indexOf for the next special), recursion
// only for the inner text of strong/em/links. `env` carries link(target).
function inline(s, env, out) {
  let i = 0;
  const n = s.length;
  while (i < n) {
    // next special char (one Irregexp scan)
    SPECIAL.lastIndex = i;
    const mm = SPECIAL.exec(s);
    const next = mm === null ? n : mm.index;
    if (next > i) { out.push(escapeHtml(s.slice(i, next))); i = next; continue; }
    if (i >= n) break;
    const c = s[i];
    if (c === '\\' && i + 1 < n && ESCAPABLE.has(s[i + 1])) { out.push(escapeHtml(s[i + 1])); i += 2; continue; }
    if (c === '`') {
      const j = s.indexOf('`', i + 1);
      if (j > i + 1) { out.push('<code>', escapeHtml(s.slice(i + 1, j)), '</code>'); i = j + 1; }
      else { out.push('`'); i++; }
      continue;
    }
    if (c === '*') {
      if (s[i + 1] === '*') {
        const j = findUnescaped(s, '**', i + 2);
        if (j > i + 2) {
          out.push('<strong>');
          inline(s.slice(i + 2, j), env, out);
          out.push('</strong>');
          i = j + 2;
        } else { out.push('**'); i += 2; }
        continue;
      }
      const j = findSingleStar(s, i + 1);
      if (j > i + 1) {
        out.push('<em>');
        inline(s.slice(i + 1, j), env, out);
        out.push('</em>');
        i = j + 1;
      } else { out.push('*'); i++; }
      continue;
    }
    if (c === '[') {
      const k = findUnescaped(s, ']', i + 1);
      const m = k !== -1 && s[k + 1] === '(' ? s.indexOf(')', k + 2) : -1;
      const target = m === -1 ? '' : s.slice(k + 2, m);
      if (target !== '' && !/\s/.test(target)) {
        out.push('<a href="', escapeHtml(env.link(target)), '">');
        inline(s.slice(i + 1, k), env, out);
        out.push('</a>');
        i = m + 1;
        continue;
      }
      out.push('[');
      i++;
      continue;
    }
    out.push(escapeHtml(c));
    i++;
  }
}

function renderMarkdown(src, env) {
  const lines = src.split('\n');
  const blocks = [];
  const toc = [];
  let excerpt = null;
  const ids = new Set();
  let i = 0;
  const L = lines.length;
  while (i < L) {
    const line = lines[i];
    if (isBlank(line)) { i++; continue; }
    const lang = isFenceOpen(line);
    if (lang !== null) {
      const body = [];
      i++;
      while (i < L && !isFenceClose(lines[i])) body.push(lines[i++]);
      i++;
      const cls = lang === '' ? '' : ` class="language-${escapeHtml(lang)}"`;
      let code = '';
      for (let b = 0; b < body.length; b++) code += escapeHtml(body[b]) + '\n';
      blocks.push(`<pre><code${cls}>${code}</code></pre>`);
      continue;
    }
    const heading = headingOf(line);
    if (heading !== null) {
      const id = uniqueSlug(slugify(heading.text), ids);
      const out = [];
      inline(heading.text, env, out);
      const html = out.join('');
      blocks.push(`<h${heading.depth} id="${id}">${html}</h${heading.depth}>`);
      if (heading.depth === 2 || heading.depth === 3) toc.push({ level: heading.depth, id, html });
      i++;
      continue;
    }
    if (isHr(line)) { blocks.push('<hr>'); i++; continue; }
    if (line[0] === '>') {
      const body = [];
      while (i < L && lines[i][0] === '>') body.push(lines[i++].replace(/^> ?/, ''));
      const kept = [];
      for (let b = 0; b < body.length; b++) { const t = trimLine(body[b]); if (t !== '') kept.push(t); }
      if (kept.length === 0) blocks.push('<blockquote>\n</blockquote>');
      else {
        const out = [];
        inline(kept.join('\n'), env, out);
        blocks.push(`<blockquote>\n<p>${out.join('')}</p>\n</blockquote>`);
      }
      continue;
    }
    // list?
    let isBullet = false, isOrdered = false, ordStart = 1, itemText = null;
    if ((line.startsWith('- ') || line.startsWith('* '))) { isBullet = true; itemText = line.slice(2); }
    else {
      // digits + ". "
      let d = 0;
      while (d < line.length && line[d] >= '0' && line[d] <= '9') d++;
      if (d >= 1 && d <= 9 && line[d] === '.' && line[d + 1] === ' ') { isOrdered = true; ordStart = Number(line.slice(0, d)); itemText = line.slice(d + 2); }
    }
    if (isBullet || isOrdered) {
      const items = [];
      let cur = [trimLine(itemText)];
      i++;
      while (i < L) {
        const ln = lines[i];
        let mText = null, mOrd = false;
        if (isOrdered) {
          let d = 0;
          while (d < ln.length && ln[d] >= '0' && ln[d] <= '9') d++;
          if (d >= 1 && d <= 9 && ln[d] === '.' && ln[d + 1] === ' ') { mOrd = true; mText = ln.slice(d + 2); }
        } else if (ln.startsWith('- ') || ln.startsWith('* ')) { mText = ln.slice(2); }
        if (mText !== null) { items.push(cur.join('\n')); cur = [trimLine(mText)]; i++; }
        else if (ln.length >= 3 && ln[0] === ' ' && ln[1] === ' ' && ln[2] !== ' ' && ln[2] !== '\t' && trimLine(ln) !== '') { cur.push(trimLine(ln)); i++; }
        else if (ln.length >= 2 && ln[0] === ' ' && ln[1] === ' ' && (ln[2] === ' ' || ln[2] === '\t')) {
          // "two or more spaces then a non-space" — check properly below
          let k = 0; while (k < ln.length && ln[k] === ' ') k++;
          if (k >= 2 && k < ln.length && ln[k] !== ' ' && ln[k] !== '\t') { cur.push(trimLine(ln)); i++; }
          else break;
        } else break;
      }
      items.push(cur.join('\n'));
      const tag = isOrdered ? 'ol' : 'ul';
      const open = isOrdered && ordStart !== 1 ? `<ol start="${ordStart}">` : `<${tag}>`;
      let html = `${open}\n`;
      for (let it = 0; it < items.length; it++) {
        const out = [];
        inline(items[it], env, out);
        html += `<li>${out.join('')}</li>\n`;
      }
      html += `</${tag}>`;
      blocks.push(html);
      continue;
    }
    // paragraph
    const body = [trimLine(line)];
    i++;
    while (i < L && !endsParagraph(lines[i])) body.push(trimLine(lines[i++]));
    const text = body.join('\n');
    const out = [];
    inline(text, env, out);
    const html = out.join('');
    if (excerpt === null) excerpt = html;
    blocks.push(`<p>${html}</p>`);
  }
  return { html: blocks.length === 0 ? '' : blocks.join('\n') + '\n', toc, excerpt: excerpt ?? '' };
}

// ---- Templates: parse (ported from makefun-site's lib/template.js), then compile ----
class TemplateError extends Error {}
const PATH_RE = /^[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*$/;
const FOR_RE = /^for ([A-Za-z_][A-Za-z0-9_]*) in (\S+)$/;
const IF_RE = /^if (not )?(\S+)$/;
const INCLUDE_RE = /^include "([a-z0-9-]+)"$/;

function pathOf(name, text) {
  if (!PATH_RE.test(text)) throw new TemplateError(`template "${name}": ${JSON.stringify(text)} is not a path`);
  return text.split('.');
}

function tokenize(name, src) {
  const tokens = [];
  let text = '';
  let i = 0;
  while (i < src.length) {
    const open = src.indexOf('{', i);
    if (open === -1 || open === src.length - 1) { text += src.slice(i); break; }
    const next = src[open + 1];
    if (next !== '{' && next !== '%') { text += src.slice(i, open + 1); i = open + 1; continue; }
    text += src.slice(i, open);
    if (next === '{') {
      const raw = src[open + 2] === '{';
      const close = src.indexOf(raw ? '}}}' : '}}', open + (raw ? 3 : 2));
      if (close === -1) throw new TemplateError(`template "${name}": a ${raw ? '{{{' : '{{'} is not closed`);
      if (text !== '') tokens.push({ t: 'text', text });
      text = '';
      tokens.push({ t: raw ? 'raw' : 'var', path: pathOf(name, src.slice(open + (raw ? 3 : 2), close).trim()) });
      i = close + (raw ? 3 : 2);
      continue;
    }
    const close = src.indexOf('%}', open + 2);
    if (close === -1) throw new TemplateError(`template "${name}": a {% is not closed`);
    const end = close + 2;
    const lineStart = src.lastIndexOf('\n', open - 1) + 1;
    const nl = src.indexOf('\n', end);
    const lineEnd = nl === -1 ? src.length : nl;
    const standalone = /^[ \t]*$/.test(src.slice(lineStart, open)) && /^[ \t]*$/.test(src.slice(end, lineEnd));
    if (standalone) text = text.slice(0, text.length - (open - lineStart));
    if (text !== '') tokens.push({ t: 'text', text });
    text = '';
    tokens.push({ t: 'tag', tag: src.slice(open + 2, close).trim() });
    i = standalone ? (nl === -1 ? src.length : nl + 1) : end;
  }
  if (text !== '') tokens.push({ t: 'text', text });
  return tokens;
}

function parseTemplate(name, src) {
  const root = { kind: 'root', body: [] };
  const stack = [root];
  let body = root.body;
  for (const tok of tokenize(name, src)) {
    if (tok.t === 'text') body.push({ kind: 'text', text: tok.text });
    else if (tok.t === 'var' || tok.t === 'raw') body.push({ kind: tok.t, path: tok.path });
    else {
      const tag = tok.tag;
      const top = stack[stack.length - 1];
      let m;
      if ((m = FOR_RE.exec(tag)) !== null) {
        const node = { kind: 'for', name: m[1], path: pathOf(name, m[2]), body: [] };
        body.push(node); stack.push(node); body = node.body;
      } else if ((m = IF_RE.exec(tag)) !== null) {
        const node = { kind: 'if', not: m[1] !== undefined, path: pathOf(name, m[2]), then: [], else: null };
        body.push(node); stack.push(node); body = node.then;
      } else if (tag === 'else') {
        if (top.kind !== 'if' || top.else !== null) throw new TemplateError(`template "${name}": an else outside an if`);
        top.else = []; body = top.else;
      } else if (tag === 'endfor' || tag === 'endif') {
        if (top.kind !== (tag === 'endfor' ? 'for' : 'if')) throw new TemplateError(`template "${name}": an ${tag} with no open ${tag.slice(3)}`);
        stack.pop();
        const parent = stack[stack.length - 1];
        body = parent.kind === 'root' ? parent.body : parent.kind === 'for' ? parent.body : parent.else ?? parent.then;
      } else if ((m = INCLUDE_RE.exec(tag)) !== null) body.push({ kind: 'include', name: m[1] });
      else throw new TemplateError(`template "${name}": {% ${tag} %} is not a tag`);
    }
  }
  if (stack.length > 1) throw new TemplateError(`template "${name}": a ${stack[stack.length - 1].kind} is not closed`);
  return root.body;
}

function textOf(v) {
  if (typeof v === 'string') return v;
  if (typeof v === 'number') return String(v);
  if (typeof v === 'boolean') return v ? 'true' : 'false';
  if (Array.isArray(v)) return v.map(textOf).join(', ');
  return '';
}
function truthy(v) {
  if (Array.isArray(v)) return v.length > 0;
  return v !== undefined && v !== null && v !== false && v !== '' && v !== 0;
}
function g(v, k) {
  return v !== null && typeof v === 'object' && Object.hasOwn(v, k) ? v[k] : undefined;
}

// Roots a template tree reads, ignoring names bound by enclosing for-loops.
function collectRoots(nodes, bound, roots, includes) {
  for (const n of nodes) {
    if (n.kind === 'var' || n.kind === 'raw' || n.kind === 'if') {
      if (!bound.has(n.path[0])) roots.add(n.path[0]);
    } else if (n.kind === 'for') {
      if (!bound.has(n.path[0])) roots.add(n.path[0]);
      const b2 = new Set(bound); b2.add(n.name);
      collectRoots(n.body, b2, roots, includes);
    } else if (n.kind === 'if') {
      // handled above for the path; branches below
    }
    if (n.kind === 'if') { collectRoots(n.then, bound, roots, includes); if (n.else) collectRoots(n.else, bound, roots, includes); }
    else if (n.kind === 'include') includes.add(n.name);
  }
}

// Compile a parsed template to a function (V, L, D, T, C, o) appending to o[].
function compileTemplate(nodes, invariant, templates) {
  let uid = 0;
  const code = [''];
  function emit(s) { code[code.length - 1] += s; }
  // Resolve a path to a JS expression. `stack` = loop contexts [{name, elem, idx, len, loopObj}].
  function pathExpr(path, stack) {
    const first = path[0];
    let expr;
    if (first === 'loop') {
      for (let s = stack.length - 1; s >= 0; s--) {
        const ctx = stack[s];
        if (path.length === 1) return `L.loop`;
        if (path.length === 2) {
          if (path[1] === 'index') return `(${ctx.idx}+1)`;
          if (path[1] === 'first') return `(${ctx.idx}===0)`;
          if (path[1] === 'last') return `(${ctx.idx}===${ctx.len}-1)`;
          if (path[1] === 'length') return `${ctx.len}`;
        }
        break;
      }
      expr = 'L.loop';
      for (let k = 1; k < path.length; k++) expr = `g(${expr},${JSON.stringify(path[k])})`;
      return expr;
    }
    for (let s = stack.length - 1; s >= 0; s--) {
      if (stack[s].name === first) { expr = stack[s].elem; break; }
    }
    if (expr === undefined) {
      if (first === 'site' || first === 'tags' || first === 'recent' || first === 'page' || first === 'posts' || first === 'pager' || first === 'tag') expr = `V.${first}`;
      else expr = `L.${first}`;
    }
    for (let k = 1; k < path.length; k++) expr = `g(${expr},${JSON.stringify(path[k])})`;
    return expr;
  }
  function genNodes(nodes, stack) {
    for (const n of nodes) {
      if (n.kind === 'text') emit(`o.push(${JSON.stringify(n.text)});`);
      else if (n.kind === 'var') emit(`o.push(esc(txt(${pathExpr(n.path, stack)})));`);
      else if (n.kind === 'raw') emit(`o.push(txt(${pathExpr(n.path, stack)}));`);
      else if (n.kind === 'if') {
        emit(`if(${n.not ? '!' : ''}truthy(${pathExpr(n.path, stack)})){`);
        genNodes(n.then, stack);
        emit('}');
        if (n.else) { emit('else{'); genNodes(n.else, stack); emit('}'); }
      } else if (n.kind === 'for') {
        const id = ++uid;
        const a = `_a${id}`, nn = `_n${id}`, ii = `_i${id}`, x = `_x${id}`, lp = `_lp${id}`, oldV = `_ov${id}`, oldL = `_ol${id}`;
        emit(`{const ${a}=${pathExpr(n.path, stack)};if(Array.isArray(${a})){const ${nn}=${a}.length;const ${lp}={index:0,first:true,last:false,length:${nn}};const ${oldL}=L.loop;const ${oldV}=L.${n.name};L.loop=${lp};for(let ${ii}=0;${ii}<${nn};${ii}++){const ${x}=${a}[${ii}];${lp}.index=${ii}+1;${lp}.first=${ii}===0;${lp}.last=${ii}===${nn}-1;L.${n.name}=${x};`);
        genNodes(n.body, [...stack, { name: n.name, elem: x, idx: ii, len: nn, loopObj: lp }]);
        emit(`}L.loop=${oldL};if(${oldV}===undefined)delete L.${n.name};else L.${n.name}=${oldV};}}`);
      } else if (n.kind === 'include') {
        if (invariant.has(n.name)) emit(`o.push(C.get(${JSON.stringify(n.name)}));`);
        else { const fid = ++uid; emit(`if(D<8){const _f${fid}=T.get(${JSON.stringify(n.name)});if(_f${fid})_f${fid}(V,L,D+1,T,C,o,esc,txt,truthy,g);}`); }
      }
    }
  }
  genNodes(nodes, []);
  const src = code[0];
  // eslint-disable-next-line no-new-func
  return new Function('V', 'L', 'D', 'T', 'C', 'o', 'esc', 'txt', 'truthy', 'g', src);
}

// ---- Build pipeline (fused port of makefun-site's index.js) ----
function resolvePath(dir, rel) {
  const parts = dir === '' ? [] : dir.split('/');
  for (const seg of rel.split('/')) {
    if (seg === '' || seg === '.') continue;
    if (seg === '..') {
      if (parts.length === 0) return null;
      parts.pop();
    } else parts.push(seg);
  }
  return parts.join('/');
}
const dirOf = (path) => (path.includes('/') ? path.slice(0, path.lastIndexOf('/')) : '');

export function run(input) {
  slugCache = new Map();
  const linkCache = new Map();
  const base = input.site.base_url.replace(/\/+$/, '');
  const report = [];
  const templates = new Map();
  const parsed = new Map();
  for (const name of Object.keys(input.templates).sort()) {
    try {
      const tree = parseTemplate(name, input.templates[name]);
      parsed.set(name, tree);
    } catch (err) {
      if (!(err instanceof TemplateError)) throw err;
      report.push(err.message);
    }
  }
  // Invariant templates: only site/tags/recent (transitively).
  const invariant = new Set();
  const invMemo = new Map();
  function isInvariant(name, seen) {
    if (invMemo.has(name)) return invMemo.get(name);
    if (seen.has(name)) return true;
    const tree = parsed.get(name);
    if (!tree) return false;
    seen.add(name);
    const roots = new Set();
    const includes = new Set();
    collectRoots(tree, new Set(), roots, includes);
    let ok = true;
    for (const r of roots) if (r !== 'site' && r !== 'tags' && r !== 'recent') { ok = false; break; }
    if (ok) for (const inc of includes) if (!isInvariant(inc, seen)) { ok = false; break; }
    seen.delete(name);
    invMemo.set(name, ok);
    return ok;
  }
  for (const name of parsed.keys()) if (isInvariant(name, new Set())) invariant.add(name);
  const compiled = new Map();
  for (const [name, tree] of parsed) compiled.set(name, compileTemplate(tree, invariant, compiled));
  // Render invariant templates once (site/tags/recent only) — filled in layouts().
  const invCache = new Map();

  // 1. Sources, sorted by path in code-unit order.
  const files = [...input.files].sort((x, y) => (x.path < y.path ? -1 : x.path > y.path ? 1 : 0));
  let sources = [];
  const statics = [];
  for (const f of files) {
    if (f.path.endsWith('.md')) sources.push({ path: f.path, text: f.content.indexOf('\r') === -1 ? f.content : f.content.replace(/\r\n?/g, '\n') });
    else statics.push({ path: f.path, content: f.content });
  }
  // 2. Front matter, in source order.
  const kept = [];
  for (const s of sources) {
    const fm = parseFrontMatter(s.text);
    if ('error' in fm) { report.push(`refused ${s.path}: ${fm.error}`); continue; }
    s.data = fm.data;
    s.body = fm.body;
    s.post = s.path.startsWith('posts/');
    let own;
    for (const d of RESERVED) if (d !== 'posts' && s.path.startsWith(`${d}/`)) { own = d; break; }
    if (own !== undefined) { report.push(`refused ${s.path}: ${own}/ is the generator's own`); continue; }
    if (s.post && !Object.hasOwn(s.data, 'date')) { report.push(`refused ${s.path}: a post needs a date`); continue; }
    const layout = s.data.layout ?? (s.post ? 'post' : 'page');
    if (!parsed.has(layout)) { report.push(`refused ${s.path}: layout "${layout}" is no template that parses`); continue; }
    s.layout = layout;
    if (s.data.draft === true) { report.push(`draft ${s.path}`); continue; }
    kept.push(s);
  }
  sources = kept;
  // 3. Permalinks.
  const taken = new Map();
  const bySource = new Map();
  for (const s of sources) {
    const dir = s.post ? 'posts' : dirOf(s.path);
    let set = taken.get(dir);
    if (!set) { set = new Set(dir === '' ? RESERVED : []); taken.set(dir, set); }
    const slug = uniqueSlug(s.data.slug ?? slugify(s.data.title), set);
    s.slug = slug;
    s.outDir = dir === '' ? slug : `${dir}/${slug}`;
    s.url = `${base}/${s.outDir}/`;
    bySource.set(s.path, s);
  }
  // 4. Markdown.
  const staticPaths = new Set(statics.map((f) => f.path));
  const broken = new Set();
  for (const s of sources) {
    const dir = dirOf(s.path);
    const env = {
      link: (target) => {
        if (/^[A-Za-z][A-Za-z0-9+.-]*:/.test(target) || target.startsWith('/') || target.startsWith('#')) return target;
        const ck = `${dir}\0${target}`;
        const hit = linkCache.get(ck);
        if (hit !== undefined) return hit;
        const hash = target.indexOf('#');
        const rel = hash === -1 ? target : target.slice(0, hash);
        const frag = hash === -1 ? '' : target.slice(hash);
        const path = rel === '' ? null : resolvePath(dir, rel);
        let out;
        let ok = false;
        if (path !== null && bySource.has(path)) { out = `${bySource.get(path).url}${frag}`; ok = true; }
        else if (path !== null && staticPaths.has(path)) { out = `${base}/${path}${frag}`; ok = true; }
        else { broken.add(`broken-link ${s.path} -> ${target}`); out = target; }
        if (ok) linkCache.set(ck, out);
        return out;
      },
    };
    const md = renderMarkdown(s.body, env);
    s.html = md.html;
    s.toc = md.toc;
    s.excerpt = md.excerpt;
  }
  // 5. Collections.
  const posts = sources.filter((s) => s.post);
  posts.sort((x, y) => (y.data.date < x.data.date ? -1 : y.data.date > x.data.date ? 1 : x.slug < y.slug ? -1 : x.slug > y.slug ? 1 : 0));
  posts.forEach((p, k) => {
    p.newer = k === 0 ? null : posts[k - 1];
    p.older = k === posts.length - 1 ? null : posts[k + 1];
  });
  // 6. Tags.
  const bySlug = new Map();
  const ownTags = (s) => {
    const seen = new Set();
    const out = [];
    for (const name of s.data.tags ?? []) {
      const sl = slugify(name);
      if (!seen.has(sl)) { seen.add(sl); out.push(name); }
    }
    return out;
  };
  for (const p of posts) {
    p.tagRefs = [];
    for (const name of ownTags(p)) {
      const slug = slugify(name);
      let tag = bySlug.get(slug);
      if (!tag) { tag = { name, slug, url: `${base}/tags/${slug}/`, posts: [] }; bySlug.set(slug, tag); }
      tag.posts.push(p);
      p.tagRefs.push(tag);
    }
  }
  for (const s of sources) {
    if (s.post) continue;
    s.tagRefs = ownTags(s).map((name) => {
      const sl = slugify(name);
      return bySlug.get(sl) ?? { name, slug: sl, url: '', posts: [] };
    });
  }
  const tags = [...bySlug.values()].sort((x, y) => y.posts.length - x.posts.length || (x.slug < y.slug ? -1 : x.slug > y.slug ? 1 : 0));
  // Cards / pages.
  const linkOf = (s) => (s === null ? null : { title: s.data.title, url: s.url, date: s.data.date ?? '', date_long: s.data.date === undefined ? '' : longDate(s.data.date) });
  const cardCache = new Map();
  const cardOf = (s) => {
    let c = cardCache.get(s);
    if (!c) {
      c = {
        title: s.data.title,
        url: s.url,
        date: s.data.date ?? '',
        date_long: s.data.date === undefined ? '' : longDate(s.data.date),
        summary: s.data.summary ?? '',
        excerpt: s.excerpt,
        reading_minutes: Math.max(1, Math.ceil(wordsOf(s.body) / 200)),
        tags: s.tagRefs.map((t) => ({ name: t.name, slug: t.slug, url: t.url })),
      };
      cardCache.set(s, c);
    }
    return c;
  };
  const pageOf = (s) => {
    const c = cardOf(s);
    if (!Object.hasOwn(c, 'content')) {
      c.slug = s.slug;
      c.content = s.html;
      c.toc = s.toc;
      c.newer = linkOf(s.newer ?? null);
      c.older = linkOf(s.older ?? null);
      c.is_post = s.post;
    }
    return c;
  };
  // 7. Layouts.
  const output = {};
  const site = { title: input.site.title, base_url: base };
  const allTags = tags.map((t) => ({ name: t.name, slug: t.slug, url: t.url, count: t.posts.length }));
  const recent = posts.slice(0, input.site.recent).map((p) => cardOf(p));
  const tagOf = (t) => ({ name: t.name, slug: t.slug, url: t.url, count: t.posts.length, posts: t.posts.map((p) => cardOf(p)) });
  // Fill the invariant cache now that site/tags/recent exist, in
  // dependency order (an invariant template may include another).
  {
    const V0 = { site, tags: allTags, recent };
    const order = [];
    const seenOrd = new Set();
    const visit = (name) => {
      if (seenOrd.has(name) || !invariant.has(name)) return;
      seenOrd.add(name);
      const incs = new Set();
      collectRoots(parsed.get(name), new Set(), new Set(), incs);
      for (const inc of incs) visit(inc);
      order.push(name);
    };
    for (const name of invariant) visit(name);
    for (const name of order) {
      const fn = compiled.get(name);
      const o = [];
      fn(V0, Object.create(null), 0, compiled, invCache, o, escapeHtml, textOf, truthy, g);
      invCache.set(name, o.join(''));
    }
  }
  const V = { site, tags: allTags, recent, page: undefined, posts: undefined, pager: undefined, tag: undefined };
  const render = (name, vars) => {
    const fn = compiled.get(name);
    if (!fn) return null;
    V.page = vars.page;
    V.posts = vars.posts;
    V.pager = vars.pager;
    V.tag = vars.tag;
    const o = [];
    fn(V, Object.create(null), 0, compiled, invCache, o, escapeHtml, textOf, truthy, g);
    return o.join('');
  };
  for (const s of sources) output[`${s.outDir}/index.html`] = render(s.layout, { page: pageOf(s) });
  const per = input.site.per_page;
  const pages = Math.max(1, Math.ceil(posts.length / per));
  const indexUrl = (k) => (k === 1 ? `${base}/` : `${base}/page/${k}/`);
  for (let k = 1; k <= pages; k++) {
    const html = render('index', {
      posts: posts.slice((k - 1) * per, k * per).map((p) => cardOf(p)),
      pager: { number: k, total: pages, url: indexUrl(k), newer_url: k === 1 ? '' : indexUrl(k - 1), older_url: k === pages ? '' : indexUrl(k + 1) },
    });
    if (html !== null) output[k === 1 ? 'index.html' : `page/${k}/index.html`] = html;
  }
  for (const t of tags) {
    const html = render('tag', { tag: tagOf(t) });
    if (html !== null) output[`tags/${t.slug}/index.html`] = html;
  }
  const tagsHtml = render('tags', {});
  if (tagsHtml !== null) output['tags/index.html'] = tagsHtml;
  for (const f of statics) {
    if (Object.hasOwn(output, f.path)) report.push(`shadowed ${f.path}: a built page has its path`);
    else output[f.path] = f.content;
  }
  // 8. Report.
  const lines = [...report, ...broken].sort();
  const pageCount = sources.filter((s) => !s.post).length;
  lines.push(`built ${pageCount} pages, ${posts.length} posts, ${tags.length} tags and ${statics.length} static files`);
  output['build-report.txt'] = `${lines.join('\n')}\n`;
  return output;
}
