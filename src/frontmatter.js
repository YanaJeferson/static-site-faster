// makefun-site's front matter parser, exactly as vendor/makefun-site/lib/frontmatter.js,
// with line scanning and no regular expression on the common path.

const LINE_KEYS = new Set(['title', 'date', 'tags', 'draft', 'layout', 'slug', 'summary']);
const DATE = /^(\d{4})-(\d{2})-(\d{2})$/;
const NAME = /^[a-z0-9-]+$/;
const SLUG = /^[a-z0-9]+(?:-[a-z0-9]+)*$/;
const TAG = /^[^"[\]]+$/;

function daysIn(y, m) {
  if (m === 2) return (y % 4 === 0 && y % 100 !== 0) || y % 400 === 0 ? 29 : 28;
  return m === 4 || m === 6 || m === 9 || m === 11 ? 30 : 31;
}

function stringOf(raw) {
  if (raw.charCodeAt(0) === 34) {
    if (raw.length < 2 || raw.charCodeAt(raw.length - 1) !== 34) return { error: 'a quoted string is not closed' };
    let out = '';
    for (let i = 1; i < raw.length - 1; i++) {
      const c = raw.charCodeAt(i);
      if (c === 92) {
        const n = raw.charCodeAt(i + 1);
        if ((n !== 34 && n !== 92) || i + 1 === raw.length - 1) return { error: 'a quoted string holds an escape other than \\" or \\\\' };
        out += String.fromCharCode(n);
        i++;
      } else if (c === 34) return { error: 'a quoted string holds a stray quote' };
      else out += String.fromCharCode(c);
    }
    return { value: out };
  }
  if (raw.charCodeAt(0) === 91) return { error: 'a bare string opens on [' };
  return { value: raw };
}

function valueOf(key, raw) {
  switch (key) {
    case 'title':
    case 'summary': {
      const s = stringOf(raw);
      return s.error !== undefined ? { error: `key "${key}": ${s.error}` } : s;
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
      if (raw.charCodeAt(0) !== 91 || raw.charCodeAt(raw.length - 1) !== 93) return { error: 'key "tags": not a list [a, b]' };
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

/** A source's front matter and body, or the first reason it is refused. */
export function parseFrontMatter(text) {
  const n = text.length;
  let firstEnd = text.indexOf('\n');
  const first = firstEnd === -1 ? text : text.slice(0, firstEnd);
  if (first !== '---') return { error: 'no front matter' };
  if (firstEnd === -1) return { error: 'the front matter is not closed' };

  let closeStart = -1;
  let closeEnd = -1;
  let i = firstEnd + 1;
  while (i <= n) {
    let j = text.indexOf('\n', i);
    const last = j === -1;
    if (last) j = n;
    if (j - i === 3 && text.charCodeAt(i) === 45 && text.charCodeAt(i + 1) === 45 && text.charCodeAt(i + 2) === 45) {
      closeStart = i;
      closeEnd = j;
      break;
    }
    if (last) break;
    i = j + 1;
  }
  if (closeStart === -1) return { error: 'the front matter is not closed' };

  const data = { title: undefined, date: undefined, tags: undefined, draft: undefined, layout: undefined, slug: undefined, summary: undefined };
  const seen = new Set();
  i = firstEnd + 1;
  while (i < closeStart) {
    let j = text.indexOf('\n', i);
    if (j === -1 || j > closeStart) j = closeStart;
    const lineEnd = j;
    // blank (spaces and tabs only) or a comment
    let blank = true;
    for (let k = i; k < lineEnd; k++) {
      const c = text.charCodeAt(k);
      if (c !== 32 && c !== 9) {
        blank = false;
        break;
      }
    }
    if (!blank && text.charCodeAt(i) !== 35) {
      // ^([a-z_]+):(?: (.*))?$
      let colon = -1;
      for (let k = i; k < lineEnd; k++) {
        if (text.charCodeAt(k) === 58) {
          colon = k;
          break;
        }
      }
      let valid = colon > i;
      if (valid) {
        for (let k = i; k < colon; k++) {
          const c = text.charCodeAt(k);
          if (!((c >= 97 && c <= 122) || c === 95)) {
            valid = false;
            break;
          }
        }
      }
      if (valid && colon + 1 < lineEnd && text.charCodeAt(colon + 1) !== 32) valid = false;
      if (!valid) return { error: `line ${countLine(text, i)} is not "key: value"` };
      const key = text.slice(i, colon);
      let raw = colon + 1 >= lineEnd ? '' : text.slice(colon + 2, lineEnd);
      let end = raw.length;
      while (end > 0) {
        const c = raw.charCodeAt(end - 1);
        if (c !== 32 && c !== 9) break;
        end--;
      }
      if (end !== raw.length) raw = raw.slice(0, end);
      if (!LINE_KEYS.has(key)) return { error: `unknown key "${key}"` };
      if (seen.has(key)) return { error: `key "${key}" is given twice` };
      if (raw === '') return { error: `key "${key}" has no value` };
      const v = valueOf(key, raw);
      if (v.error !== undefined) return v;
      seen.add(key);
      data[key] = v.value;
    }
    if (j === closeStart) break;
    i = j + 1;
  }
  if (data.title === undefined) return { error: 'no title' };
  return { data, body: closeEnd >= n ? '' : text.slice(closeEnd + 1) };
}

/** The 1-based line number of offset `at`, counted from the opening "---". */
function countLine(text, at) {
  let line = 1;
  for (let i = 0; i < at; i++) if (text.charCodeAt(i) === 10) line++;
  return line;
}
