// makefun-site's Markdown: its own block lexer and inline renderer, exactly as
// vendor/makefun-site/lib/markdown.js, rewritten to scan lines and runs.

import { escapeHtml, slugify, uniqueSlug, isSpaceCode } from './text.js';

const NEEDS_ESCAPE = new Uint8Array(128);
NEEDS_ESCAPE[38] = 1; // &
NEEDS_ESCAPE[60] = 1; // <
NEEDS_ESCAPE[62] = 1; // >
NEEDS_ESCAPE[34] = 1; // "
NEEDS_ESCAPE[39] = 1; // '

const ESCAPABLE = new Uint8Array(128);
for (const c of '\\`*_[]()#+-.!>') ESCAPABLE[c.charCodeAt(0)] = 1;

/** The characters `inline` stops at: the escape table, an escape, a code span, emphasis and a link. */
const MARK = new Uint8Array(128);
MARK[92] = 1; // backslash
MARK[96] = 1; // `
MARK[42] = 1; // *
MARK[91] = 1; // [
MARK[38] = 1;
MARK[60] = 1;
MARK[62] = 1;
MARK[34] = 1;
MARK[39] = 1;

function isBlank(line) {
  for (let i = 0; i < line.length; i++) {
    const c = line.charCodeAt(i);
    if (c !== 32 && c !== 9) return false;
  }
  return true;
}

/** `line` with spaces and tabs at either end dropped. */
function trim(line) {
  let a = 0;
  let b = line.length;
  while (a < b) {
    const c = line.charCodeAt(a);
    if (c !== 32 && c !== 9) break;
    a++;
  }
  while (b > a) {
    const c = line.charCodeAt(b - 1);
    if (c !== 32 && c !== 9) break;
    b--;
  }
  return a === 0 && b === line.length ? line : line.slice(a, b);
}

/** `{ lang }` when `line` opens a fence, else null. */
function fenceOpen(line) {
  if (line.length < 3 || line.charCodeAt(0) !== 96 || line.charCodeAt(1) !== 96 || line.charCodeAt(2) !== 96) return null;
  let k = 3;
  while (k < line.length) {
    const c = line.charCodeAt(k);
    if (!((c >= 65 && c <= 90) || (c >= 97 && c <= 122) || (c >= 48 && c <= 57) || c === 95 || c === 43 || c === 45)) break;
    k++;
  }
  for (let j = k; j < line.length; j++) {
    const c = line.charCodeAt(j);
    if (c !== 32 && c !== 9) return null;
  }
  return { lang: line.slice(3, k) };
}

function fenceClose(line) {
  if (line.length < 3 || line.charCodeAt(0) !== 96 || line.charCodeAt(1) !== 96 || line.charCodeAt(2) !== 96) return false;
  for (let i = 3; i < line.length; i++) {
    const c = line.charCodeAt(i);
    if (c !== 32 && c !== 9) return false;
  }
  return true;
}

/** A heading line's level and text, or null. */
function headingOf(line) {
  let k = 0;
  while (k < line.length && line.charCodeAt(k) === 35) k++;
  if (k === 0 || k > 6 || k === line.length) return null;
  const c = line.charCodeAt(k);
  if (c !== 32 && c !== 9) return null;
  let s = k;
  while (s < line.length) {
    const d = line.charCodeAt(s);
    if (d !== 32 && d !== 9) break;
    s++;
  }
  let e = line.length;
  while (e > s) {
    const d = line.charCodeAt(e - 1);
    if (d !== 32 && d !== 9) break;
    e--;
  }
  if (e <= s) return null;
  return { depth: k, text: line.slice(s, e) };
}

function isHr(line) {
  if (line.length < 3) return false;
  const a = line.charCodeAt(0);
  if (a === 45) {
    if (line.charCodeAt(1) !== 45 || line.charCodeAt(2) !== 45) return false;
  } else if (a === 42) {
    if (line.charCodeAt(1) !== 42 || line.charCodeAt(2) !== 42) return false;
  } else return false;
  for (let i = 3; i < line.length; i++) {
    const c = line.charCodeAt(i);
    if (c !== 32 && c !== 9) return false;
  }
  return true;
}

/** The text of a bullet item on `line`, or null. */
function bulletOf(line) {
  if (line.length < 2 || line.charCodeAt(1) !== 32) return null;
  const c = line.charCodeAt(0);
  return c === 45 || c === 42 ? line.slice(2) : null;
}

/** `{ start, text }` for an ordered item on `line`, or null. */
function orderedOf(line) {
  const n = line.length;
  let k = 0;
  while (k < n) {
    const c = line.charCodeAt(k);
    if (c < 48 || c > 57) break;
    k++;
    if (k > 9) return null;
  }
  if (k === 0 || k + 2 > n) return null;
  if (line.charCodeAt(k) !== 46 || line.charCodeAt(k + 1) !== 32) return null;
  return { start: Number(line.slice(0, k)), text: line.slice(k + 2) };
}

/** A line of two or more spaces and then a non-space continues a list item. */
function isContinued(line) {
  let k = 0;
  const n = line.length;
  while (k < n && line.charCodeAt(k) === 32) k++;
  if (k < 2 || k >= n) return false;
  return !isSpaceCode(line.charCodeAt(k));
}

function endsParagraph(line) {
  const len = line.length;
  if (len === 0) return true;
  const c = line.charCodeAt(0);
  if (c === 32 || c === 9) return isBlank(line);
  if (c === 96) return fenceOpen(line) !== null;
  if (c === 35) return headingOf(line) !== null;
  if (c === 45 || c === 42) return isHr(line);
  return false;
}

/** The first index from `from` where `s` holds `what` outside a backslash escape, or -1. */
function findUnescaped(s, what, from) {
  let j = s.indexOf(what, from);
  while (j !== -1) {
    let backslashes = 0;
    for (let k = j - 1; k >= from && s.charCodeAt(k) === 92; k--) backslashes++;
    if (backslashes % 2 === 0) return j;
    j = s.indexOf(what, j + 1);
  }
  return -1;
}

/** The first single * from `from` (not part of ** and not escaped), or -1. */
function findSingleStar(s, from) {
  const n = s.length;
  for (let j = from; j < n; j++) {
    const c = s.charCodeAt(j);
    if (c === 92 && j + 1 < n && ESCAPABLE[s.charCodeAt(j + 1)] === 1) {
      j++;
      continue;
    }
    if (c !== 42) continue;
    if (s.charCodeAt(j + 1) === 42) {
      j++;
      continue;
    }
    return j;
  }
  return -1;
}

function hasWhitespace(s) {
  for (let i = 0; i < s.length; i++) if (isSpaceCode(s.charCodeAt(i))) return true;
  return false;
}

/** `s` rendered inline; `env.link(target)` is a link's href. */
export function inline(s, env) {
  const n = s.length;
  const parts = [];
  let i = 0;
  while (i < n) {
    const start = i;
    while (i < n) {
      const c = s.charCodeAt(i);
      if (c < 128 && MARK[c] === 1) break;
      i++;
    }
    if (i > start) parts.push(s.slice(start, i));
    if (i >= n) break;
    const c = s.charCodeAt(i);
    if (c === 92) {
      const next = i + 1 < n ? s.charCodeAt(i + 1) : -1;
      if (next >= 0 && next < 128 && ESCAPABLE[next] === 1) {
        parts.push(next === 62 ? '&gt;' : s[i + 1]);
        i += 2;
      } else {
        parts.push('\\');
        i++;
      }
      continue;
    }
    if (c === 96) {
      const j = s.indexOf('`', i + 1);
      if (j > i + 1) {
        parts.push('<code>', escapeHtml(s.slice(i + 1, j)), '</code>');
        i = j + 1;
      } else {
        parts.push('`');
        i++;
      }
      continue;
    }
    if (c === 42) {
      if (s.charCodeAt(i + 1) === 42) {
        const j = findUnescaped(s, '**', i + 2);
        if (j > i + 2) {
          parts.push('<strong>', inline(s.slice(i + 2, j), env), '</strong>');
          i = j + 2;
        } else {
          parts.push('**');
          i += 2;
        }
        continue;
      }
      const j = findSingleStar(s, i + 1);
      if (j > i + 1) {
        parts.push('<em>', inline(s.slice(i + 1, j), env), '</em>');
        i = j + 1;
      } else {
        parts.push('*');
        i++;
      }
      continue;
    }
    if (c === 91) {
      const k = findUnescaped(s, ']', i + 1);
      const m = k !== -1 && s.charCodeAt(k + 1) === 40 ? s.indexOf(')', k + 2) : -1;
      const target = m === -1 ? '' : s.slice(k + 2, m);
      if (target !== '' && !hasWhitespace(target)) {
        parts.push('<a href="', escapeHtml(env.link(target)), '">', inline(s.slice(i + 1, k), env), '</a>');
        i = m + 1;
        continue;
      }
      parts.push('[');
      i++;
      continue;
    }
    parts.push(c === 38 ? '&amp;' : c === 60 ? '&lt;' : c === 62 ? '&gt;' : c === 34 ? '&quot;' : '&#39;');
    i++;
  }
  return parts.join('');
}

/** `src` rendered: { html, toc, excerpt }. */
export function renderMarkdown(src, env) {
  const lines = src.split('\n');
  const count = lines.length;
  const blocks = [];
  const ids = new Set();
  const toc = [];
  let excerpt = null;
  let i = 0;
  while (i < count) {
    const line = lines[i];
    if (isBlank(line)) {
      i++;
      continue;
    }
    const fence = fenceOpen(line);
    if (fence !== null) {
      const body = [];
      i++;
      while (i < count && !fenceClose(lines[i])) body.push(lines[i++]);
      i++;
      const cls = fence.lang === '' ? '' : ` class="language-${escapeHtml(fence.lang)}"`;
      blocks.push(`<pre><code${cls}>${body.length === 0 ? '' : `${escapeHtml(body.join('\n'))}\n`}</code></pre>`);
      continue;
    }
    const heading = headingOf(line);
    if (heading !== null) {
      const id = uniqueSlug(slugify(heading.text), ids);
      const html = inline(heading.text, env);
      blocks.push(`<h${heading.depth} id="${id}">${html}</h${heading.depth}>`);
      if (heading.depth === 2 || heading.depth === 3) toc.push({ level: heading.depth, id, html });
      i++;
      continue;
    }
    if (isHr(line)) {
      blocks.push('<hr>');
      i++;
      continue;
    }
    if (line.charCodeAt(0) === 62) {
      const body = [];
      while (i < count && lines[i].charCodeAt(0) === 62) {
        const l = lines[i++];
        const text = trim(l.slice(l.charCodeAt(1) === 32 ? 2 : 1));
        if (text !== '') body.push(text);
      }
      blocks.push(body.length === 0 ? '<blockquote>\n</blockquote>' : `<blockquote>\n<p>${inline(body.join('\n'), env)}</p>\n</blockquote>`);
      continue;
    }
    const bullet = bulletOf(line);
    const ordered = bullet === null ? orderedOf(line) : null;
    if (bullet !== null || ordered !== null) {
      const isOrdered = ordered !== null;
      const items = [];
      if (isOrdered) {
        while (i < count) {
          const m = orderedOf(lines[i]);
          if (m !== null) {
            items.push([trim(m.text)]);
            i++;
          } else if (items.length > 0 && isContinued(lines[i])) {
            items[items.length - 1].push(trim(lines[i]));
            i++;
          } else break;
        }
      } else {
        while (i < count) {
          const m = bulletOf(lines[i]);
          if (m !== null) {
            items.push([trim(m)]);
            i++;
          } else if (items.length > 0 && isContinued(lines[i])) {
            items[items.length - 1].push(trim(lines[i]));
            i++;
          } else break;
        }
      }
      const tag = isOrdered ? 'ol' : 'ul';
      const start = isOrdered ? ordered.start : 1;
      const open = isOrdered && start !== 1 ? `<ol start="${start}">` : `<${tag}>`;
      let body = '';
      for (const item of items) {
        body += `<li>${inline(item.join('\n'), env)}</li>\n`;
      }
      blocks.push(`${open}\n${body}</${tag}>`);
      continue;
    }
    const body = [trim(line)];
    i++;
    while (i < count && !endsParagraph(lines[i])) body.push(trim(lines[i++]));
    const html = inline(body.join('\n'), env);
    if (excerpt === null) excerpt = html;
    blocks.push(`<p>${html}</p>`);
  }
  return { html: blocks.length === 0 ? '' : `${blocks.join('\n')}\n`, toc, excerpt: excerpt === null ? '' : excerpt };
}
