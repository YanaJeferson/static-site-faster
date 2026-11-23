// Text rules of makefun-site 1.0.0: HTML escaping, slugs, long dates and word counts.
// Optimised for the hot paths (escape per run, ASCII slug fast path) while keeping the
// exact semantics of the reference (vendor/makefun-site/lib/text.js).

const AMP = 38, LT = 60, GT = 62, QUOT = 34, APOS = 39;

/** 1 for the five characters the escape table names. */
const NEEDS_ESCAPE = new Uint8Array(128);
NEEDS_ESCAPE[AMP] = 1;
NEEDS_ESCAPE[LT] = 1;
NEEDS_ESCAPE[GT] = 1;
NEEDS_ESCAPE[QUOT] = 1;
NEEDS_ESCAPE[APOS] = 1;

/** `text` with & < > " and ' escaped, scanning for runs instead of one character at a time. */
export function escapeHtml(text) {
  const n = text.length;
  let i = 0;
  while (i < n) {
    const c = text.charCodeAt(i);
    if (c < 128 && NEEDS_ESCAPE[c] === 1) break;
    i++;
  }
  if (i === n) return text;
  let out = text.slice(0, i);
  while (i < n) {
    const c = text.charCodeAt(i);
    if (c < 128 && NEEDS_ESCAPE[c] === 1) {
      out += c === AMP ? '&amp;' : c === LT ? '&lt;' : c === GT ? '&gt;' : c === QUOT ? '&quot;' : '&#39;';
      i++;
    } else {
      const start = i;
      i++;
      while (i < n) {
        const d = text.charCodeAt(i);
        if (d < 128 && NEEDS_ESCAPE[d] === 1) break;
        i++;
      }
      out += text.slice(start, i);
    }
  }
  return out;
}

/** The most characters a slug keeps before a collision suffix. */
export const SLUG_MAX = 64;

const MARK = /\p{M}/gu;

/** The slug of `text` (README "Text"), with an ASCII fast path. */
export function slugify(text) {
  let plain;
  let ascii = true;
  for (let i = 0; i < text.length; i++) {
    if (text.charCodeAt(i) > 127) {
      ascii = false;
      break;
    }
  }
  plain = ascii ? text : text.normalize('NFKD').replace(MARK, '');
  let out = '';
  let gap = false;
  for (let i = 0; i < plain.length; i++) {
    let c = plain.charCodeAt(i);
    if (c < 128) {
      if (c >= 65 && c <= 90) c += 32;
      if ((c >= 97 && c <= 122) || (c >= 48 && c <= 57)) {
        if (gap) out += '-';
        gap = false;
        out += String.fromCharCode(c);
      } else {
        gap = out.length > 0;
      }
      continue;
    }
    // A code point outside ASCII never becomes a kept character; step a whole pair.
    const cp = plain.codePointAt(i);
    if (cp > 0xffff) i++;
    gap = out.length > 0;
  }
  if (out.length > SLUG_MAX) out = out.slice(0, SLUG_MAX);
  while (out.length > 0 && out.charCodeAt(out.length - 1) === 45) out = out.slice(0, -1);
  return out === '' ? 'untitled' : out;
}

/** `slug` made unique against `taken` (and added to it). */
export function uniqueSlug(slug, taken) {
  let out = slug;
  for (let n = 2; taken.has(out); n++) out = `${slug}-${n}`;
  taken.add(out);
  return out;
}

const MONTHS = ['January', 'February', 'March', 'April', 'May', 'June', 'July', 'August', 'September', 'October', 'November', 'December'];

/** A YYYY-MM-DD date in words, from the string alone. */
export function longDate(date) {
  const m = (date.charCodeAt(5) - 48) * 10 + (date.charCodeAt(6) - 48);
  const d = (date.charCodeAt(8) - 48) * 10 + (date.charCodeAt(9) - 48);
  const y = date.slice(0, 4);
  return `${d} ${MONTHS[m - 1]} ${y}`;
}

/** Whether code point `c` is JavaScript's `\s`. */
function isSpaceCode(c) {
  if (c < 128) return c === 32 || (c >= 9 && c <= 13);
  return c === 0xa0 || c === 0x1680 || (c >= 0x2000 && c <= 0x200a) || c === 0x2028 || c === 0x2029 || c === 0x202f || c === 0x205f || c === 0x3000 || c === 0xfeff;
}

/** The number of runs of characters other than whitespace. */
export function wordsOf(text) {
  let n = 0;
  let inWord = false;
  const len = text.length;
  for (let i = 0; i < len; i++) {
    const c = text.charCodeAt(i);
    const space = c < 128 ? c === 32 || (c >= 9 && c <= 13) : isSpaceCode(c);
    if (space) inWord = false;
    else if (!inWord) {
      inWord = true;
      n++;
    }
  }
  return n;
}

/** Exposed to the Markdown and front-matter scanners. */
export { isSpaceCode };
