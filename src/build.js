// makefun-site 1.0.0's build pipeline, in its order and with its exact reports.
// Same semantics as vendor/makefun-site/index.js; the hot passes cache what the reference
// recomputes (date strings, tag slugs, link resolutions) inside one build.

import { parseFrontMatter } from './frontmatter.js';
import { renderMarkdown } from './markdown.js';
import { parseTemplate, renderTemplate, TemplateError } from './template.js';
import { longDate, slugify, uniqueSlug, wordsOf } from './text.js';

export const REPORT_FILE = 'build-report.txt';
export const RESERVED = Object.freeze(['posts', 'tags', 'page']);

const byCodeUnits = (a, b) => (a < b ? -1 : a > b ? 1 : 0);

function dirOf(path) {
  const at = path.lastIndexOf('/');
  return at === -1 ? '' : path.slice(0, at);
}

/** `rel` from the directory `dir` of a source, or null when it climbs out of the root. */
function resolvePath(dir, rel) {
  const parts = [];
  if (dir !== '') {
    let start = 0;
    while (true) {
      const end = dir.indexOf('/', start);
      if (end === -1) {
        parts.push(dir.slice(start));
        break;
      }
      parts.push(dir.slice(start, end));
      start = end + 1;
    }
  }
  let start = 0;
  while (start <= rel.length) {
    let end = rel.indexOf('/', start);
    if (end === -1) end = rel.length;
    const len = end - start;
    if (len === 0) {
      // an empty step is skipped
    } else if (len === 1 && rel.charCodeAt(start) === 46) {
      // "."
    } else if (len === 2 && rel.charCodeAt(start) === 46 && rel.charCodeAt(start + 1) === 46) {
      if (parts.length === 0) return null;
      parts.pop();
    } else parts.push(rel.slice(start, end));
    if (end === rel.length) break;
    start = end + 1;
  }
  return parts.join('/');
}

/** A target that starts with a scheme (`[A-Za-z][A-Za-z0-9+.-]*:`), kept as written. */
function hasScheme(target) {
  const n = target.length;
  let i = 0;
  let c = target.charCodeAt(0);
  if (!((c >= 65 && c <= 90) || (c >= 97 && c <= 122))) return false;
  i = 1;
  while (i < n) {
    c = target.charCodeAt(i);
    if (c === 58) return true;
    if (!((c >= 65 && c <= 90) || (c >= 97 && c <= 122) || (c >= 48 && c <= 57) || c === 43 || c === 46 || c === 45)) return false;
    i++;
  }
  return false;
}

function trimSlash(url) {
  let end = url.length;
  while (end > 0 && url.charCodeAt(end - 1) === 47) end--;
  return end === url.length ? url : url.slice(0, end);
}

function readSources(b) {
  const files = b.input.files.slice();
  files.sort((x, y) => byCodeUnits(x.path, y.path));
  for (let i = 0; i < files.length; i++) {
    const f = files[i];
    if (f.path.endsWith('.md')) {
      b.sources.push({
        path: f.path,
        text: f.content.indexOf('\r') === -1 ? f.content : f.content.replace(/\r\n?/g, '\n'),
        data: null,
        body: null,
        post: false,
        layout: null,
        slug: null,
        outDir: null,
        url: null,
        html: null,
        toc: null,
        excerpt: null,
        newer: null,
        older: null,
        tagRefs: null,
        words: 0,
        card: null,
        link: null,
      });
    } else {
      b.statics.push({ path: f.path, content: f.content });
    }
  }
}

function frontMatter(b) {
  const kept = [];
  for (let i = 0; i < b.sources.length; i++) {
    const s = b.sources[i];
    const fm = parseFrontMatter(s.text);
    if (fm.error !== undefined) {
      b.report.push(`refused ${s.path}: ${fm.error}`);
      continue;
    }
    s.data = fm.data;
    s.body = fm.body;
    s.post = s.path.length >= 6 && s.path.slice(0, 6) === 'posts/';
    if (s.path.length >= 5 && s.path.slice(0, 5) === 'tags/') {
      b.report.push(`refused ${s.path}: tags/ is the generator's own`);
      continue;
    }
    if (s.path.length >= 5 && s.path.slice(0, 5) === 'page/') {
      b.report.push(`refused ${s.path}: page/ is the generator's own`);
      continue;
    }
    if (s.post && s.data.date === undefined) {
      b.report.push(`refused ${s.path}: a post needs a date`);
      continue;
    }
    const layout = s.data.layout ?? (s.post ? 'post' : 'page');
    if (!b.templates.has(layout)) {
      b.report.push(`refused ${s.path}: layout "${layout}" is no template that parses`);
      continue;
    }
    s.layout = layout;
    if (s.data.draft === true) {
      b.report.push(`draft ${s.path}`);
      continue;
    }
    kept.push(s);
  }
  b.sources = kept;
}

function permalinks(b) {
  const taken = new Map();
  for (let i = 0; i < b.sources.length; i++) {
    const s = b.sources[i];
    const dir = s.post ? 'posts' : dirOf(s.path);
    let set = taken.get(dir);
    if (set === undefined) {
      set = new Set(dir === '' ? RESERVED : []);
      taken.set(dir, set);
    }
    const slug = uniqueSlug(s.data.slug ?? slugify(s.data.title), set);
    s.slug = slug;
    s.outDir = dir === '' ? slug : `${dir}/${slug}`;
    s.url = `${b.base}/${s.outDir}/`;
    b.bySource.set(s.path, s);
  }
}

function markdown(b) {
  const staticPaths = new Set();
  for (let i = 0; i < b.statics.length; i++) staticPaths.add(b.statics[i].path);
  const linkCache = new Map();
  let currentDir = '';
  let currentSrc = '';
  const env = {
    link(target) {
      const dir = currentDir;
      if (hasScheme(target) || target.charCodeAt(0) === 47 || target.charCodeAt(0) === 35) return target;
      let dirCache = linkCache.get(dir);
      if (dirCache === undefined) {
        dirCache = new Map();
        linkCache.set(dir, dirCache);
      }
      let cached = dirCache.get(target);
      if (cached === undefined) {
        const hash = target.indexOf('#');
        const rel = hash === -1 ? target : target.slice(0, hash);
        const frag = hash === -1 ? '' : target.slice(hash);
        const path = rel === '' ? null : resolvePath(dir, rel);
        if (path !== null && b.bySource.has(path)) cached = b.bySource.get(path).url + frag;
        else if (path !== null && staticPaths.has(path)) cached = `${b.base}/${path}${frag}`;
        else cached = null;
        dirCache.set(target, cached);
      }
      if (cached === null) {
        b.broken.add(`broken-link ${currentSrc} -> ${target}`);
        return target;
      }
      return cached;
    },
  };
  for (let i = 0; i < b.sources.length; i++) {
    const s = b.sources[i];
    currentDir = dirOf(s.path);
    currentSrc = s.path;
    const md = renderMarkdown(s.body, env);
    s.html = md.html;
    s.toc = md.toc;
    s.excerpt = md.excerpt;
    s.words = wordsOf(s.body);
  }
}

function collections(b) {
  const posts = [];
  for (let i = 0; i < b.sources.length; i++) if (b.sources[i].post) posts.push(b.sources[i]);
  posts.sort((x, y) => byCodeUnits(y.data.date, x.data.date) || byCodeUnits(x.slug, y.slug));
  b.posts = posts;
  for (let k = 0; k < posts.length; k++) {
    posts[k].newer = k === 0 ? null : posts[k - 1];
    posts[k].older = k === posts.length - 1 ? null : posts[k + 1];
  }
}

function tags(b) {
  const bySlug = new Map();
  const slugCache = new Map();
  const slugOf = (name) => {
    let s = slugCache.get(name);
    if (s === undefined) {
      s = slugify(name);
      slugCache.set(name, s);
    }
    return s;
  };
  const ownTags = (s) => {
    const list = s.data.tags ?? [];
    const seen = new Set();
    const out = [];
    for (let i = 0; i < list.length; i++) {
      const name = list[i];
      const slug = slugOf(name);
      if (seen.has(slug)) continue;
      seen.add(slug);
      out.push(name);
    }
    return out;
  };
  for (let p = 0; p < b.posts.length; p++) {
    const post = b.posts[p];
    const refs = [];
    post.tagRefs = refs;
    const names = ownTags(post);
    for (let i = 0; i < names.length; i++) {
      const name = names[i];
      const slug = slugOf(name);
      let tag = bySlug.get(slug);
      if (tag === undefined) {
        tag = { name, slug, url: `${b.base}/tags/${slug}/`, posts: [], count: 0 };
        bySlug.set(slug, tag);
      }
      tag.posts.push(post);
      refs.push(tag);
    }
  }
  for (let i = 0; i < b.sources.length; i++) {
    const s = b.sources[i];
    if (s.post) continue;
    const names = ownTags(s);
    const refs = [];
    for (let j = 0; j < names.length; j++) {
      const name = names[j];
      const slug = slugOf(name);
      const tag = bySlug.get(slug);
      refs.push(tag ?? { name, slug, url: '', posts: [], count: 0 });
    }
    s.tagRefs = refs;
  }
  b.tags = [...bySlug.values()].sort((x, y) => y.posts.length - x.posts.length || byCodeUnits(x.slug, y.slug));
  for (let i = 0; i < b.tags.length; i++) b.tags[i].count = b.tags[i].posts.length;
}

function dateLong(s) {
  if (s.data.date === undefined) return '';
  let d = s.dateLong;
  if (d === undefined) {
    d = longDate(s.data.date);
    s.dateLong = d;
  }
  return d;
}

function linkOf(s) {
  if (s === null) return null;
  let l = s.link;
  if (l === null) {
    l = { title: s.data.title, url: s.url, date: s.data.date ?? '', date_long: dateLong(s) };
    s.link = l;
  }
  return l;
}

function cardOf(s) {
  let c = s.card;
  if (c === null) {
    c = {
      title: s.data.title,
      url: s.url,
      date: s.data.date ?? '',
      date_long: dateLong(s),
      summary: s.data.summary ?? '',
      excerpt: s.excerpt,
      reading_minutes: Math.max(1, Math.ceil(s.words / 200)),
      tags: s.tagRefs,
    };
    s.card = c;
  }
  return c;
}

function pageOf(s) {
  const c = cardOf(s);
  return {
    title: c.title,
    url: c.url,
    date: c.date,
    date_long: c.date_long,
    summary: c.summary,
    excerpt: c.excerpt,
    reading_minutes: c.reading_minutes,
    tags: c.tags,
    slug: s.slug,
    content: s.html,
    toc: s.toc,
    newer: linkOf(s.newer),
    older: linkOf(s.older),
    is_post: s.post,
  };
}

function layouts(b) {
  const cards = new Map();
  for (let i = 0; i < b.posts.length; i++) cards.set(b.posts[i], cardOf(b.posts[i]));
  b.cards = cards;
  const site = { title: b.input.site.title, base_url: b.base };
  const allTags = b.tags;
  const recent = [];
  const recentCount = Math.min(b.input.site.recent, b.posts.length);
  for (let i = 0; i < recentCount; i++) recent.push(cards.get(b.posts[i]));
  const ctx = { templates: b.templates };
  const render = (name, vars) => {
    const nodes = b.templates.get(name);
    if (nodes === undefined) return null;
    const root = { site, tags: allTags, recent, page: undefined, posts: undefined, pager: undefined, tag: undefined };
    for (const key in vars) root[key] = vars[key];
    return renderTemplate(nodes, root, ctx);
  };
  for (let i = 0; i < b.sources.length; i++) {
    const s = b.sources[i];
    b.output[`${s.outDir}/index.html`] = render(s.layout, { page: pageOf(s) });
  }
  const per = b.input.site.per_page;
  const pages = Math.max(1, Math.ceil(b.posts.length / per));
  const indexUrl = (k) => (k === 1 ? `${b.base}/` : `${b.base}/page/${k}/`);
  for (let k = 1; k <= pages; k++) {
    const slice = [];
    const from = (k - 1) * per;
    const to = Math.min(k * per, b.posts.length);
    for (let i = from; i < to; i++) slice.push(cards.get(b.posts[i]));
    const html = render('index', {
      posts: slice,
      pager: { number: k, total: pages, url: indexUrl(k), newer_url: k === 1 ? '' : indexUrl(k - 1), older_url: k === pages ? '' : indexUrl(k + 1) },
    });
    if (html !== null) b.output[k === 1 ? 'index.html' : `page/${k}/index.html`] = html;
  }
  for (let i = 0; i < b.tags.length; i++) {
    const t = b.tags[i];
    const posts = [];
    for (let j = 0; j < t.posts.length; j++) posts.push(cards.get(t.posts[j]));
    const html = render('tag', { tag: { name: t.name, slug: t.slug, url: t.url, posts, count: t.posts.length } });
    if (html !== null) b.output[`tags/${t.slug}/index.html`] = html;
  }
  const tagsHtml = render('tags', {});
  if (tagsHtml !== null) b.output['tags/index.html'] = tagsHtml;
  for (let i = 0; i < b.statics.length; i++) {
    const f = b.statics[i];
    if (Object.hasOwn(b.output, f.path)) b.report.push(`shadowed ${f.path}: a built page has its path`);
    else b.output[f.path] = f.content;
  }
}

function report(b) {
  const lines = b.report.concat([...b.broken]);
  lines.sort(byCodeUnits);
  let pages = 0;
  for (let i = 0; i < b.sources.length; i++) if (!b.sources[i].post) pages++;
  lines.push(`built ${pages} pages, ${b.posts.length} posts, ${b.tags.length} tags and ${b.statics.length} static files`);
  b.output[REPORT_FILE] = `${lines.join('\n')}\n`;
}

/** One site's output tree: path -> text. */
export function build(input) {
  const templates = new Map();
  const reportLines = [];
  const names = Object.keys(input.templates).sort(byCodeUnits);
  for (let i = 0; i < names.length; i++) {
    const name = names[i];
    try {
      templates.set(name, parseTemplate(name, input.templates[name]));
    } catch (err) {
      if (!(err instanceof TemplateError)) throw err;
      reportLines.push(err.message);
    }
  }
  const b = {
    input,
    base: trimSlash(input.site.base_url),
    templates,
    sources: [],
    statics: [],
    bySource: new Map(),
    broken: new Set(),
    report: reportLines,
    posts: [],
    tags: [],
    cards: null,
    output: {},
  };
  readSources(b);
  frontMatter(b);
  permalinks(b);
  markdown(b);
  collections(b);
  tags(b);
  layouts(b);
  report(b);
  return b.output;
}
