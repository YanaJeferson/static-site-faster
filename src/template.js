// makefun-site's template dialect: same tokenizer and parser as
// vendor/makefun-site/lib/template.js, rendered from pre-split paths into one output array.
// Every node has the same hidden class, which keeps the render loop monomorphic.

import { escapeHtml } from './text.js';

export const INCLUDE_DEPTH = 8;

const PATH = /^[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*$/;
const FOR = /^for ([A-Za-z_][A-Za-z0-9_]*) in (\S+)$/;
const IF = /^if (not )?(\S+)$/;
const INCLUDE = /^include "([a-z0-9-]+)"$/;

export class TemplateError extends Error {}

function pathOf(name, text) {
  if (!PATH.test(text)) throw new TemplateError(`template "${name}": ${JSON.stringify(text)} is not a path`);
  return text.split('.');
}

/** One node shape for every kind: { k, t, p, not, then, els, name, body }. */
function node(k, t, p, not, then, els, name, body) {
  return { k, t, p, not, then, els, name, body };
}

function tokenize(name, src) {
  const tokens = [];
  let text = '';
  let i = 0;
  while (i < src.length) {
    const open = src.indexOf('{', i);
    if (open === -1 || open === src.length - 1) {
      text += src.slice(i);
      break;
    }
    const next = src[open + 1];
    if (next !== '{' && next !== '%') {
      text += src.slice(i, open + 1);
      i = open + 1;
      continue;
    }
    text += src.slice(i, open);
    if (next === '{') {
      const raw = src[open + 2] === '{';
      const close = src.indexOf(raw ? '}}}' : '}}', open + (raw ? 3 : 2));
      if (close === -1) throw new TemplateError(`template "${name}": a ${raw ? '{{{' : '{{'} is not closed`);
      if (text !== '') tokens.push({ t: 0, text });
      text = '';
      tokens.push({ t: raw ? 2 : 1, path: pathOf(name, src.slice(open + (raw ? 3 : 2), close).trim()) });
      i = close + (raw ? 3 : 2);
      continue;
    }
    const close = src.indexOf('%}', open + 2);
    if (close === -1) throw new TemplateError(`template "${name}": a {% is not closed`);
    const end = close + 2;
    const lineStart = src.lastIndexOf('\n', open - 1) + 1;
    const nl = src.indexOf('\n', end);
    const lineEnd = nl === -1 ? src.length : nl;
    let standalone = true;
    for (let k = lineStart; k < open; k++) {
      const c = src.charCodeAt(k);
      if (c !== 32 && c !== 9) {
        standalone = false;
        break;
      }
    }
    if (standalone) {
      for (let k = end; k < lineEnd; k++) {
        const c = src.charCodeAt(k);
        if (c !== 32 && c !== 9) {
          standalone = false;
          break;
        }
      }
    }
    if (standalone) text = text.slice(0, text.length - (open - lineStart));
    if (text !== '') tokens.push({ t: 0, text });
    text = '';
    tokens.push({ t: 3, tag: src.slice(open + 2, close).trim() });
    i = standalone ? (nl === -1 ? src.length : nl + 1) : end;
  }
  if (text !== '') tokens.push({ t: 0, text });
  return tokens;
}

export function parseTemplate(name, src) {
  const root = node(-1);
  root.body = [];
  const stack = [root];
  let body = root.body;
  for (const tok of tokenize(name, src)) {
    if (tok.t === 0) body.push(node(0, tok.text, null, null, null, null, null, null));
    else if (tok.t === 1 || tok.t === 2) body.push(node(tok.t, null, tok.path, null, null, null, null, null));
    else {
      const tag = tok.tag;
      const top = stack[stack.length - 1];
      let m;
      if ((m = FOR.exec(tag)) !== null) {
        const n = node(4, null, pathOf(name, m[2]), false, null, null, m[1], []);
        body.push(n);
        stack.push(n);
        body = n.body;
      } else if ((m = IF.exec(tag)) !== null) {
        const n = node(3, null, pathOf(name, m[2]), m[1] !== undefined, [], null, null, null);
        body.push(n);
        stack.push(n);
        body = n.then;
      } else if (tag === 'else') {
        if (top.k !== 3 || top.els !== null) throw new TemplateError(`template "${name}": an else outside an if`);
        top.els = [];
        body = top.els;
      } else if (tag === 'endfor' || tag === 'endif') {
        if (top.k !== (tag === 'endfor' ? 4 : 3)) throw new TemplateError(`template "${name}": an ${tag} with no open ${tag.slice(3)}`);
        stack.pop();
        const parent = stack[stack.length - 1];
        body = parent.k === -1 ? parent.body : parent.k === 4 ? parent.body : parent.els ?? parent.then;
      } else if ((m = INCLUDE.exec(tag)) !== null) body.push(node(5, null, null, null, null, null, m[1], null));
      else throw new TemplateError(`template "${name}": {% ${tag} %} is not a tag`);
    }
  }
  if (stack.length > 1) throw new TemplateError(`template "${name}": a ${stack[stack.length - 1].k === 4 ? 'for' : 'if'} is not closed`);
  return root.body;
}

/** A scope has one shape: { vars, name, value, loop, parent }. */
export function rootScope(vars) {
  return { vars, name: null, value: undefined, loop: null, parent: null };
}

function lookup(scope, path) {
  const first = path[0];
  let s = scope;
  let v;
  let found = false;
  while (s !== null) {
    if (s.vars !== null) {
      if (Object.hasOwn(s.vars, first)) {
        v = s.vars[first];
        found = true;
        break;
      }
    } else {
      if (first === 'loop' && s.loop !== null) {
        v = s.loop;
        found = true;
        break;
      }
      if (s.name === first) {
        v = s.value;
        found = true;
        break;
      }
    }
    s = s.parent;
  }
  if (!found) return undefined;
  for (let k = 1; k < path.length; k++) {
    if (v === null || typeof v !== 'object' || !Object.hasOwn(v, path[k])) return undefined;
    v = v[path[k]];
  }
  return v;
}

function textOf(v) {
  if (typeof v === 'string') return v;
  if (typeof v === 'number') return String(v);
  if (typeof v === 'boolean') return v ? 'true' : 'false';
  if (Array.isArray(v)) {
    let out = '';
    for (let i = 0; i < v.length; i++) out += (i === 0 ? '' : ', ') + textOf(v[i]);
    return out;
  }
  return '';
}

function truthy(v) {
  if (Array.isArray(v)) return v.length > 0;
  return v !== undefined && v !== null && v !== false && v !== '' && v !== 0;
}

function renderNodes(nodes, scope, ctx, depth, out) {
  for (let j = 0; j < nodes.length; j++) {
    const n = nodes[j];
    switch (n.k) {
      case 0:
        out.push(n.t);
        break;
      case 1:
        out.push(escapeHtml(textOf(lookup(scope, n.p))));
        break;
      case 2:
        out.push(textOf(lookup(scope, n.p)));
        break;
      case 3:
        if (truthy(lookup(scope, n.p)) !== n.not) renderNodes(n.then, scope, ctx, depth, out);
        else if (n.els !== null) renderNodes(n.els, scope, ctx, depth, out);
        break;
      case 4: {
        const xs = lookup(scope, n.p);
        if (!Array.isArray(xs)) break;
        const len = xs.length;
        for (let k = 0; k < len; k++) {
          const loop = { index: k + 1, first: k === 0, last: k === len - 1, length: len };
          renderNodes(n.body, { vars: null, name: n.name, value: xs[k], loop, parent: scope }, ctx, depth, out);
        }
        break;
      }
      case 5: {
        const t = ctx.templates.get(n.name);
        if (t !== undefined && depth < INCLUDE_DEPTH) renderNodes(t, scope, ctx, depth + 1, out);
        break;
      }
    }
  }
}

/** `nodes` rendered with the top-level `vars`; `ctx` holds the templates. */
export function renderTemplate(nodes, vars, ctx) {
  const out = [];
  renderNodes(nodes, rootScope(vars), ctx, 0, out);
  return out.join('');
}
