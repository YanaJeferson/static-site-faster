// A native (Node-API) implementation of makefun-site 1.0.0's build, byte-identical to the
// reference and fast enough to compete in the timing heat. The JS entry (solve.js) stays as
// the readable reference; this file is the same pipeline compiled.
//
// Build: g++ -O3 -std=c++20 -fPIC -shared -static-libstdc++ -static-libgcc \
//          -I/usr/include/node engine.cpp -o engine.node

#ifndef NATIVE_TEST
#include <node_api.h>
#endif
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <charconv>
#include <array>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <memory>
#include <deque>
#ifndef NATIVE_TEST
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#endif
#if defined(__SSE2__)
#include <emmintrin.h>
#endif
#include <stdexcept>
#include <chrono>

#include "unicode_data.h"

using std::string;
using std::string_view;
using std::vector;
using std::unordered_map;
using std::unordered_set;

static string trimUnicode(string_view s);
static double nowMs();

static uint8_t MD_SPECIAL[128];
static uint8_t HTML_SPECIAL[128];
struct SpecialTables {
  SpecialTables() {
    for (char c : string_view("&<>\"'\\`*[")) MD_SPECIAL[(unsigned char)c] = 1;
    for (char c : string_view("&<>\"'")) HTML_SPECIAL[(unsigned char)c] = 1;
  }
} specialTablesInit;

#if defined(__SSE2__)
static inline __m128i splat(char c) { return _mm_set1_epi8(c); }
static inline int mdMask16(const char* p) {
  __m128i v = _mm_loadu_si128((const __m128i*)p);
  __m128i m = _mm_cmpeq_epi8(v, splat('&'));
  m = _mm_or_si128(m, _mm_cmpeq_epi8(v, splat('<')));
  m = _mm_or_si128(m, _mm_cmpeq_epi8(v, splat('>')));
  m = _mm_or_si128(m, _mm_cmpeq_epi8(v, splat('"')));
  m = _mm_or_si128(m, _mm_cmpeq_epi8(v, splat('\'')));
  m = _mm_or_si128(m, _mm_cmpeq_epi8(v, splat('\\')));
  m = _mm_or_si128(m, _mm_cmpeq_epi8(v, splat('`')));
  m = _mm_or_si128(m, _mm_cmpeq_epi8(v, splat('*')));
  m = _mm_or_si128(m, _mm_cmpeq_epi8(v, splat('[')));
  return _mm_movemask_epi8(m);
}
static inline int htmlMask16(const char* p) {
  __m128i v = _mm_loadu_si128((const __m128i*)p);
  __m128i m = _mm_cmpeq_epi8(v, splat('&'));
  m = _mm_or_si128(m, _mm_cmpeq_epi8(v, splat('<')));
  m = _mm_or_si128(m, _mm_cmpeq_epi8(v, splat('>')));
  m = _mm_or_si128(m, _mm_cmpeq_epi8(v, splat('"')));
  m = _mm_or_si128(m, _mm_cmpeq_epi8(v, splat('\'')));
  return _mm_movemask_epi8(m);
}
static inline int strMask16(const char* p) {
  __m128i v = _mm_loadu_si128((const __m128i*)p);
  __m128i m = _mm_cmpeq_epi8(v, splat('"'));
  m = _mm_or_si128(m, _mm_cmpeq_epi8(v, splat('\\')));
  return _mm_movemask_epi8(m);
}
static inline int jsonMask16(const char* p) {
  __m128i v = _mm_loadu_si128((const __m128i*)p);
  __m128i m = _mm_cmpeq_epi8(v, splat('"'));
  m = _mm_or_si128(m, _mm_cmpeq_epi8(v, splat('\\')));
  __m128i ctl = _mm_cmpeq_epi8(_mm_subs_epu8(v, splat(0x1F)), _mm_setzero_si128());
  m = _mm_or_si128(m, ctl);
  return _mm_movemask_epi8(m);
}
#endif


struct StringHash {
  using is_transparent = void;
  size_t operator()(string_view s) const { return std::hash<string_view>{}(s); }
  size_t operator()(const string& s) const { return std::hash<string_view>{}(s); }
};
using StringSet = std::unordered_set<string, StringHash, std::equal_to<>>;

struct Buf {
  char* p = nullptr;
  size_t len = 0;
  size_t cap = 0;
  Buf() = default;
  Buf(const Buf&) = delete;
  Buf& operator=(const Buf&) = delete;
  Buf(Buf&& o) noexcept : p(o.p), len(o.len), cap(o.cap) { o.p = nullptr; o.len = o.cap = 0; }
  Buf& operator=(Buf&& o) noexcept {
    if (this != &o) { free(p); p = o.p; len = o.len; cap = o.cap; o.p = nullptr; o.len = o.cap = 0; }
    return *this;
  }
  ~Buf() { free(p); }
  void reserve(size_t n) {
    if (n <= cap) return;
    size_t c = cap == 0 ? 256 : cap;
    while (c < n) c <<= 1;
    char* q = (char*)realloc(p, c);
    if (q == nullptr) throw std::bad_alloc();
    p = q;
    cap = c;
  }
  void put(char c) { if (len == cap) reserve(cap == 0 ? 256 : cap * 2); p[len++] = c; }
  void push_back(char c) { put(c); }
  void append(const char* s, size_t n) { reserve(len + n); if (n) memcpy(p + len, s, n); len += n; }
  void append(string_view s) { append(s.data(), s.size()); }
  Buf& operator+=(string_view s) { append(s); return *this; }
  Buf& operator+=(const char* s) { append(s, strlen(s)); return *this; }
  Buf& operator+=(const string& s) { append(s.data(), s.size()); return *this; }
  void clear() { len = 0; }
  void swap(Buf& o) { char* tp = p; p = o.p; o.p = tp; size_t tl = len; len = o.len; o.len = tl; size_t tc = cap; cap = o.cap; o.cap = tc; }
  string str() const { return p == nullptr ? string() : string(p, len); }
};

static inline uint64_t hasZeroByte8(uint64_t v) {
  return (v - 0x0101010101010101ULL) & ~v & 0x8080808080808080ULL;
}
static inline uint64_t htmlSpecialMask(uint64_t v) {
  return hasZeroByte8(v ^ 0x2626262626262626ULL) | hasZeroByte8(v ^ 0x3C3C3C3C3C3C3C3CULL) |
         hasZeroByte8(v ^ 0x3E3E3E3E3E3E3E3EULL) | hasZeroByte8(v ^ 0x2222222222222222ULL) |
         hasZeroByte8(v ^ 0x2727272727272727ULL);
}

// ------------------------------------------------------------------ text utilities

static inline bool isAsciiSpace(char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }

static bool isJsSpaceCp(uint32_t cp) {
  return cp == 0x20 || (cp >= 0x09 && cp <= 0x0D) || cp == 0xA0 || cp == 0x1680 ||
         (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 || cp == 0x202F ||
         cp == 0x205F || cp == 0x3000 || cp == 0xFEFF;
}

/** Decode one UTF-8 code point at `i`; advances i. Invalid bytes decode as themselves. */
static uint32_t utf8Next(string_view s, size_t& i) {
  unsigned char c = (unsigned char)s[i];
  if (c < 0x80) { i++; return c; }
  if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
    uint32_t cp = ((c & 0x1F) << 6) | ((unsigned char)s[i + 1] & 0x3F);
    i += 2;
    return cp;
  }
  if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
    uint32_t cp = ((c & 0x0F) << 12) | (((unsigned char)s[i + 1] & 0x3F) << 6) | ((unsigned char)s[i + 2] & 0x3F);
    i += 3;
    return cp;
  }
  if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) {
    uint32_t cp = ((c & 0x07) << 18) | (((unsigned char)s[i + 1] & 0x3F) << 12) | (((unsigned char)s[i + 2] & 0x3F) << 6) | ((unsigned char)s[i + 3] & 0x3F);
    i += 4;
    return cp;
  }
  i++;
  return c;
}

static void utf8Append(string& out, uint32_t cp) {
  if (cp < 0x80) out.push_back((char)cp);
  else if (cp < 0x800) {
    out.push_back((char)(0xC0 | (cp >> 6)));
    out.push_back((char)(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back((char)(0xE0 | (cp >> 12)));
    out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back((char)(0x80 | (cp & 0x3F)));
  } else {
    out.push_back((char)(0xF0 | (cp >> 18)));
    out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back((char)(0x80 | (cp & 0x3F)));
  }
}

static const NfkdEntry* nfkdFind(uint32_t cp) {
  uint32_t lo = 0, hi = kNfkdCount;
  while (lo < hi) {
    uint32_t mid = (lo + hi) / 2;
    if (kNfkd[mid].cp < cp) lo = mid + 1;
    else hi = mid;
  }
  if (lo < kNfkdCount && kNfkd[lo].cp == cp) return &kNfkd[lo];
  return nullptr;
}

static bool isMark(uint32_t cp) {
  uint32_t lo = 0, hi = kMarkCount;
  while (lo < hi) {
    uint32_t mid = (lo + hi) / 2;
    if (kMarks[mid].hi < cp) lo = mid + 1;
    else hi = mid;
  }
  return lo < kMarkCount && kMarks[lo].lo <= cp && cp <= kMarks[lo].hi;
}

static string escapeHtml(string_view s) {
  string out;
  out.reserve(s.size() + 16);
  size_t start = 0;
  for (size_t i = 0; i < s.size(); i++) {
    unsigned char c = (unsigned char)s[i];
    const char* rep = (c < 128 && HTML_SPECIAL[c] != 0) ? (c == '&' ? "&amp;" : c == '<' ? "&lt;" : c == '>' ? "&gt;" : c == '"' ? "&quot;" : "&#39;") : nullptr;
    if (rep != nullptr) {
      out.append(s.substr(start, i - start));
      out.append(rep);
      start = i + 1;
    }
  }
  out.append(s.substr(start));
  return out;
}

template <class O>
static void appendEscapedHtml(O& out, string_view s) {
  const char* p = s.data();
  size_t n = s.size();
  size_t start = 0;
  size_t i = 0;
#if defined(__SSE2__)
  while (i + 16 <= n) {
    int mask = htmlMask16(p + i);
    if (mask == 0) { i += 16; continue; }
    i += (size_t)__builtin_ctz((unsigned)mask);
    unsigned char c = (unsigned char)p[i];
    const char* rep = c == '&' ? "&amp;" : c == '<' ? "&lt;" : c == '>' ? "&gt;" : c == '"' ? "&quot;" : "&#39;";
    out.append(p + start, i - start);
    out.append(rep, strlen(rep));
    i++;
    start = i;
  }
#endif
  for (; i < n; i++) {
    unsigned char c = (unsigned char)p[i];
    if (c >= 128 || HTML_SPECIAL[c] == 0) continue;
    const char* rep = c == '&' ? "&amp;" : c == '<' ? "&lt;" : c == '>' ? "&gt;" : c == '"' ? "&quot;" : "&#39;";
    out.append(p + start, i - start);
    out.append(rep, strlen(rep));
    start = i + 1;
  }
  out.append(p + start, n - start);
}

static const char* kMonths[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};

static string longDate(string_view date) {
  int m = (date[5] - '0') * 10 + (date[6] - '0');
  int d = (date[8] - '0') * 10 + (date[9] - '0');
  string out = std::to_string(d);
  out.push_back(' ');
  out.append(kMonths[m - 1]);
  out.push_back(' ');
  out.append(date.substr(0, 4));
  return out;
}

static string slugify(string_view text) {
  bool ascii = true;
  for (size_t i = 0; i < text.size(); i++) if ((unsigned char)text[i] >= 0x80) { ascii = false; break; }
  string out;
  out.reserve(64);
  bool gap = false;
  if (ascii) {
    for (size_t i = 0; i < text.size(); i++) {
      unsigned char c = (unsigned char)text[i];
      unsigned char keep = 0;
      if (c >= 'A' && c <= 'Z') keep = (unsigned char)(c + 32);
      else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) keep = c;
      if (keep == 0) {
        gap = !out.empty();
      } else {
        if (gap && !out.empty()) out.push_back('-');
        gap = false;
        out.push_back((char)keep);
      }
    }
  } else {
    size_t i = 0;
    while (i < text.size()) {
      uint32_t cp = utf8Next(text, i);
      if (cp < 0x80) {
        if (cp >= 'A' && cp <= 'Z') cp += 32;
        if ((cp >= 'a' && cp <= 'z') || (cp >= '0' && cp <= '9')) {
          if (gap && !out.empty()) out.push_back('-');
          gap = false;
          out.push_back((char)cp);
        } else {
          gap = !out.empty();
        }
        continue;
      }
      const NfkdEntry* e = nfkdFind(cp);
      if (e == nullptr) {
        if (cp < 0x300 || !isMark(cp)) gap = !out.empty();
      } else {
        for (uint32_t k = 0; k < e->len; k++) {
          uint32_t d = kSeq[e->off + k];
          if (isMark(d)) continue;
          if (d >= 'A' && d <= 'Z') d += 32;
          if ((d >= 'a' && d <= 'z') || (d >= '0' && d <= '9')) {
            if (gap && !out.empty()) out.push_back('-');
            gap = false;
            utf8Append(out, d);
          } else {
            gap = !out.empty();
          }
        }
      }
    }
  }
  if (out.size() > 64) out.resize(64);
  while (!out.empty() && out.back() == '-') out.pop_back();
  return out.empty() ? string("untitled") : out;
}

static string uniqueSlug(string_view slug, StringSet& taken) {
  string out(slug);
  for (int n = 2; taken.find(out) != taken.end(); n++) {
    out.assign(slug);
    out.push_back('-');
    out.append(std::to_string(n));
  }
  taken.emplace(out);
  return out;
}

static string uniqueSlugSmall(string_view slug, vector<string>& taken) {
  string out(slug);
  for (int n = 2;; n++) {
    bool hit = false;
    for (const string& s : taken) if (s == out) { hit = true; break; }
    if (!hit) break;
    out.assign(slug);
    out.push_back('-');
    out.append(std::to_string(n));
  }
  taken.push_back(out);
  return out;
}

static int countWords(string_view s) {
  int n = 0;
  bool inWord = false;
  for (size_t i = 0; i < s.size(); i++) {
    unsigned char c = (unsigned char)s[i];
    bool space;
    if (c < 0x80) space = isAsciiSpace((char)c);
    else {
      // decode a full code point so non-ASCII whitespace is seen
      size_t j = i;
      uint32_t cp = utf8Next(s, j);
      space = isJsSpaceCp(cp);
      i = j - 1;
    }
    if (space) inWord = false;
    else if (!inWord) {
      inWord = true;
      n++;
    }
  }
  return n;
}

// ------------------------------------------------------------------ JSON input

struct CaseInput {
  string_view siteTitle;
  string_view baseUrl;
  int perPage = 0;
  int recent = 0;
  vector<std::pair<string_view, string_view>> templates;
  vector<std::pair<string_view, string_view>> files;
};

class CasesParser {
 public:
  explicit CasesParser(string_view text) : p(text) { pool.reserve(text.size() + 16); }
  vector<CaseInput> parse();
  string pool;

 private:
  string_view p;
  size_t i = 0;

  [[noreturn]] void fail(const char* what) {
    throw std::runtime_error(string("json: ") + what + " at " + std::to_string(i));
  }
  void ws() {
    while (i < p.size()) {
      char c = p[i];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') i++;
      else break;
    }
  }
  bool eat(char c) {
    if (i < p.size() && p[i] == c) { i++; return true; }
    return false;
  }
  void expect(char c) { if (!eat(c)) fail("expected character"); }
  static int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  }
  string_view str() {
    if (i >= p.size() || p[i] != '"') fail("string");
    i++;
    size_t outStart = pool.size();
    while (i < p.size()) {
      size_t start = i;
#if defined(__SSE2__)
      while (i + 16 <= p.size()) {
        int mask = strMask16(p.data() + i);
        if (mask == 0) { i += 16; continue; }
        i += (size_t)__builtin_ctz((unsigned)mask);
        break;
      }
#endif
      while (i < p.size() && p[i] != '"' && p[i] != '\\') i++;
      if (i > start) pool.append(p.substr(start, i - start));
      if (i >= p.size()) fail("unterminated string");
      char c = p[i];
      if (c == '"') { i++; return string_view(pool.data() + outStart, pool.size() - outStart); }
      i++;
      if (i >= p.size()) fail("escape");
      char e = p[i++];
      switch (e) {
        case '"': pool.push_back('"'); break;
        case '\\': pool.push_back('\\'); break;
        case '/': pool.push_back('/'); break;
        case 'b': pool.push_back('\b'); break;
        case 'f': pool.push_back('\f'); break;
        case 'n': pool.push_back('\n'); break;
        case 'r': pool.push_back('\r'); break;
        case 't': pool.push_back('\t'); break;
        case 'u': {
          if (i + 4 > p.size()) fail("unicode escape");
          uint32_t cp = 0;
          for (int k = 0; k < 4; k++) {
            int h = hex(p[i + k]);
            if (h < 0) fail("unicode escape");
            cp = cp * 16 + h;
          }
          i += 4;
          if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= p.size() && p[i] == '\\' && p[i + 1] == 'u') {
            uint32_t lo = 0;
            bool ok = true;
            for (int k = 0; k < 4; k++) {
              int h = hex(p[i + 2 + k]);
              if (h < 0) { ok = false; break; }
              lo = lo * 16 + h;
            }
            if (ok && lo >= 0xDC00 && lo <= 0xDFFF) {
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
              i += 6;
            }
          }
          if (cp == 0) pool.push_back('\0');
          else utf8Append(pool, cp);
          break;
        }
        default: fail("escape");
      }
    }
    fail("unterminated string");
  }

  double num() {
    size_t start = i;
    if (i < p.size() && p[i] == '-') i++;
    while (i < p.size() && p[i] >= '0' && p[i] <= '9') i++;
    if (i < p.size() && p[i] == '.') { i++; while (i < p.size() && p[i] >= '0' && p[i] <= '9') i++; }
    if (i < p.size() && (p[i] == 'e' || p[i] == 'E')) {
      i++;
      if (i < p.size() && (p[i] == '+' || p[i] == '-')) i++;
      while (i < p.size() && p[i] >= '0' && p[i] <= '9') i++;
    }
    if (i == start) fail("number");
    string tmp(p.substr(start, i - start));
    return std::strtod(tmp.c_str(), nullptr);
  }
  void skipValue() {
    ws();
    if (i >= p.size()) fail("value");
    char c = p[i];
    if (c == '"') { str(); return; }
    if (c == '{') {
      i++; ws();
      if (eat('}')) return;
      while (true) {
        ws(); str(); ws(); expect(':'); skipValue(); ws();
        if (eat(',')) continue;
        expect('}');
        return;
      }
    }
    if (c == '[') {
      i++; ws();
      if (eat(']')) return;
      while (true) {
        skipValue(); ws();
        if (eat(',')) continue;
        expect(']');
        return;
      }
    }
    if (p.substr(i, 4) == "true") { i += 4; return; }
    if (p.substr(i, 5) == "false") { i += 5; return; }
    if (p.substr(i, 4) == "null") { i += 4; return; }
    num();
  }
  void parseSite(CaseInput& c) {
    ws(); expect('{'); ws();
    if (eat('}')) return;
    while (true) {
      ws(); string_view k = str(); ws(); expect(':');
      if (k == "title") { ws(); c.siteTitle = str(); }
      else if (k == "base_url") { ws(); c.baseUrl = str(); }
      else if (k == "per_page") { ws(); c.perPage = (int)num(); }
      else if (k == "recent") { ws(); c.recent = (int)num(); }
      else skipValue();
      ws();
      if (eat(',')) continue;
      expect('}');
      return;
    }
  }
  void parseTemplates(CaseInput& c) {
    ws(); expect('{'); ws();
    if (eat('}')) return;
    while (true) {
      ws(); string_view k = str(); ws(); expect(':');
      ws(); string_view v = str(); c.templates.emplace_back(k, v);
      ws();
      if (eat(',')) continue;
      expect('}');
      return;
    }
  }
  void parseFiles(CaseInput& c) {
    ws(); expect('['); ws();
    if (eat(']')) return;
    while (true) {
      ws(); expect('{'); ws();
      string_view path, content;
      if (!eat('}')) {
        while (true) {
          ws(); string_view k = str(); ws(); expect(':');
          if (k == "path") { ws(); path = str(); }
          else if (k == "content") { ws(); content = str(); }
          else skipValue();
          ws();
          if (eat(',')) continue;
          expect('}');
          break;
        }
      }
      c.files.emplace_back(path, content);
      ws();
      if (eat(',')) continue;
      expect(']');
      return;
    }
  }
  void parseInput(CaseInput& c) {
    ws(); expect('{'); ws();
    if (eat('}')) return;
    while (true) {
      ws(); string_view k = str(); ws(); expect(':');
      if (k == "site") parseSite(c);
      else if (k == "templates") parseTemplates(c);
      else if (k == "files") parseFiles(c);
      else skipValue();
      ws();
      if (eat(',')) continue;
      expect('}');
      return;
    }
  }
};

vector<CaseInput> CasesParser::parse() {
  vector<CaseInput> cases;
  ws(); expect('['); ws();
  if (eat(']')) return cases;
  while (true) {
    ws(); expect('{'); ws();
    CaseInput c;
    bool hasInput = false;
    if (!eat('}')) {
      while (true) {
        ws(); string_view k = str(); ws(); expect(':');
        if (k == "input") { parseInput(c); hasInput = true; }
        else skipValue();
        ws();
        if (eat(',')) continue;
        expect('}');
        break;
      }
    }
    if (!hasInput) fail("case without input");
    cases.push_back(std::move(c));
    ws();
    if (eat(',')) continue;
    expect(']');
    return cases;
  }
}

// ------------------------------------------------------------------ output JSON

struct OutBuf {
  char* data = nullptr;
  size_t len = 0;
  size_t cap = 0;
  ~OutBuf() { free(data); }
  void ensure(size_t extra) {
    if (len + extra <= cap) return;
    size_t want = cap == 0 ? (1u << 20) : cap;
    while (want < len + extra) want <<= 1;
    char* p = (char*)realloc(data, want);
    if (p == nullptr) throw std::bad_alloc();
    data = p;
    cap = want;
  }
  void put(char c) {
    if (len == cap) ensure(1);
    data[len++] = c;
  }
  void write(const char* p, size_t n) {
    ensure(n);
    memcpy(data + len, p, n);
    len += n;
  }
  void write(string_view s) { write(s.data(), s.size()); }
};

static string jsonQuote(string_view s) {
  string out;
  out.reserve(s.size() + 2);
  out.push_back('"');
  for (char c : s) {
    if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(c); }
    else if ((unsigned char)c < 0x20) {
      char buf[8];
      snprintf(buf, sizeof buf, "\\u%04x", (unsigned char)c);
      out += buf;
    } else out.push_back(c);
  }
  out.push_back('"');
  return out;
}

static inline uint64_t hasZeroByte(uint64_t v) { return hasZeroByte8(v); }
static inline uint64_t hasLessThan32(uint64_t v) {
  return (v - 0x2020202020202020ULL) & ~v & 0x8080808080808080ULL;
}

static void jsonStringTo(OutBuf& out, string_view s) {
  size_t n = s.size();
  out.ensure(n * 6 + 2);
  char* base = out.data;
  size_t w = out.len;
  base[w++] = '"';
  const char* p = s.data();
  size_t start = 0;
  size_t i = 0;
  while (i < n) {
#if defined(__SSE2__)
    while (i + 16 <= n) {
      int mask = jsonMask16(p + i);
      if (mask == 0) { i += 16; continue; }
      i += (size_t)__builtin_ctz((unsigned)mask);
      break;
    }
#endif
    while (i < n) {
      unsigned char c = (unsigned char)p[i];
      if (c < 0x20 || c == '"' || c == '\\') break;
      i++;
    }
    if (i > start) {
      memcpy(base + w, p + start, i - start);
      w += i - start;
      start = i;
    }
    if (i >= n) break;
    unsigned char c = (unsigned char)p[i];
    switch (c) {
      case '"': base[w++] = '\\'; base[w++] = '"'; break;
      case '\\': base[w++] = '\\'; base[w++] = '\\'; break;
      case '\b': base[w++] = '\\'; base[w++] = 'b'; break;
      case '\f': base[w++] = '\\'; base[w++] = 'f'; break;
      case '\n': base[w++] = '\\'; base[w++] = 'n'; break;
      case '\r': base[w++] = '\\'; base[w++] = 'r'; break;
      case '\t': base[w++] = '\\'; base[w++] = 't'; break;
      default: {
        static const char* hex = "0123456789abcdef";
        base[w++] = '\\'; base[w++] = 'u'; base[w++] = '0'; base[w++] = '0';
        base[w++] = hex[(c >> 4) & 15]; base[w++] = hex[c & 15];
        break;
      }
    }
    i++;
    start = i;
  }
  if (start < n) {
    memcpy(base + w, p + start, n - start);
    w += n - start;
  }
  base[w++] = '"';
  out.len = w;
}

// ------------------------------------------------------------------ template values

struct Val {
  enum T : uint8_t { NUL, STR, NUM, BOOL, ARR, OBJ } t = NUL;
  string_view s;
  string owned;
  double n = 0;
  bool b = false;
  vector<Val*> arr;
  vector<std::pair<string_view, Val*>> obj;
};

class ValPool {
 public:
  Val* make() { vals.emplace_back(); return &vals.back(); }
 private:
  std::deque<Val> vals;
};

static Val* valStr(ValPool& pool, string s) {
  Val* v = pool.make();
  v->t = Val::STR;
  v->owned = std::move(s);
  v->s = v->owned;
  return v;
}
static Val* valStrV(ValPool& pool, string_view s) {
  Val* v = pool.make();
  v->t = Val::STR;
  v->s = s;
  return v;
}
static Val* valNum(ValPool& pool, double n) {
  Val* v = pool.make();
  v->t = Val::NUM;
  v->n = n;
  return v;
}
static Val* valBool(ValPool& pool, bool b) {
  Val* v = pool.make();
  v->t = Val::BOOL;
  v->b = b;
  return v;
}
static Val* valNull(ValPool& pool) { return pool.make(); }
static Val* valArr(ValPool& pool) {
  Val* v = pool.make();
  v->t = Val::ARR;
  return v;
}
static Val* valObj(ValPool& pool) {
  Val* v = pool.make();
  v->t = Val::OBJ;
  return v;
}
static void objPut(Val* o, string_view key, Val* v) { o->obj.emplace_back(key, v); }

static bool truthy(const Val* v) {
  if (v == nullptr) return false;
  switch (v->t) {
    case Val::NUL: return false;
    case Val::BOOL: return v->b;
    case Val::NUM: return v->n != 0;
    case Val::STR: return !v->s.empty();
    case Val::ARR: return !v->arr.empty();
    case Val::OBJ: return true;
  }
  return false;
}

template <class O>
static void textOf(O& out, const Val* v) {
  if (v == nullptr) return;
  switch (v->t) {
    case Val::NUL: return;
    case Val::STR: out.append(v->s.data(), v->s.size()); return;
    case Val::NUM: {
      char buf[32];
      if (v->n == (long long)v->n) {
        auto r = std::to_chars(buf, buf + sizeof buf, (long long)v->n);
        out.append(buf, r.ptr - buf);
      } else {
        auto r = std::to_chars(buf, buf + sizeof buf, v->n);
        out.append(buf, r.ptr - buf);
      }
      return;
    }
    case Val::BOOL: out.append(v->b ? "true" : "false"); return;
    case Val::ARR:
      for (size_t i = 0; i < v->arr.size(); i++) {
        if (i != 0) out.append(", ");
        textOf(out, v->arr[i]);
      }
      return;
    case Val::OBJ: return;
  }
}

// ------------------------------------------------------------------ templates

struct TemplateError : std::runtime_error {
  explicit TemplateError(const string& m) : std::runtime_error(m) {}
};

struct TNode {
  int kind = 0; // 0 text, 1 var, 2 raw, 3 if, 4 for, 5 include
  string text;      // text or include name or for variable name
  vector<string> path;
  bool notFlag = false;
  bool hasElse = false;
  vector<TNode*> body;
  vector<TNode*> els;
  // Reused values for a for loop's `loop` binding (safe: no template includes itself).
  Val* loopVal = nullptr;
  Val* loopIndex = nullptr;
  Val* loopFirst = nullptr;
  Val* loopLast = nullptr;
  Val* loopLength = nullptr;
  bool constant = false;
  const vector<TNode*>* includeNodes = nullptr;
  int includeDepth = 0;
  string constHtml;
  bool constReady = false;
  string singleVar;
  unordered_map<Val*, string> valCache;
};

struct Template {
  string name;
  bool ok = false;
  string error;
  vector<TNode*> body;
  unordered_set<string> freeVars;
  int state = 0;       // 0 unvisited, 1 visiting, 2 done
  bool cycle = false;
  bool freeConst = false;
  int maxIncludeDepth = 0;
};

class TemplatePool {
 public:
  TNode* make() { nodes.emplace_back(); return &nodes.back(); }
 private:
  std::deque<TNode> nodes;
};

static bool allSpaceTab(string_view s) {
  for (char c : s) if (c != ' ' && c != '\t') return false;
  return true;
}

static vector<string> pathOf(const string& name, string_view text) {
  bool ok = !text.empty();
  if (ok) {
    char c = text[0];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_')) ok = false;
  }
  for (size_t i = 0; ok && i < text.size(); i++) {
    char c = text[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.') continue;
    ok = false;
  }
  if (!ok) {
    string err = "template \"" + name + "\": ";
    err += jsonQuote(text);
    err += " is not a path";
    throw TemplateError(err);
  }
  vector<string> parts;
  size_t start = 0;
  while (true) {
    size_t dot = text.find('.', start);
    if (dot == string_view::npos) {
      parts.emplace_back(text.substr(start));
      break;
    }
    parts.emplace_back(text.substr(start, dot - start));
    start = dot + 1;
  }
  return parts;
}

static bool matchFor(const string& tag, string& varName, string& path) {
  if (tag.compare(0, 4, "for ") != 0) return false;
  size_t in = tag.find(" in ", 4);
  if (in == string::npos) return false;
  varName = tag.substr(4, in - 4);
  if (varName.empty()) return false;
  char first = varName[0];
  if (!((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') || first == '_')) return false;
  for (char c : varName) {
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
  }
  path = tag.substr(in + 4);
  return !path.empty() && path.find_first_of(" \t\n\r\v\f") == string::npos;
}

static bool matchIf(const string& tag, bool& notFlag, string& path) {
  if (tag.compare(0, 3, "if ") != 0) return false;
  string rest = tag.substr(3);
  notFlag = false;
  if (rest.compare(0, 4, "not ") == 0) {
    notFlag = true;
    rest = rest.substr(4);
  }
  if (rest.empty() || rest.find_first_of(" \t\n\r\v\f") != string::npos) return false;
  path = rest;
  return true;
}

static bool matchInclude(const string& tag, string& name) {
  if (tag.compare(0, 9, "include \"") != 0) return false;
  if (tag.size() < 11 || tag.back() != '"') return false;
  name = tag.substr(9, tag.size() - 10);
  if (name.empty()) return false;
  for (char c : name) if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
  return true;
}

static vector<TNode*> parseTemplate(const string& name, string_view src, TemplatePool& pool) {
  vector<TNode*> root;
  vector<TNode*> stack;  // open for/if nodes; empty means root
  vector<TNode*>* body = &root;

  string text;
  size_t i = 0;
  auto flushText = [&]() {
    if (!text.empty()) {
      TNode* n = pool.make();
      n->kind = 0;
      n->text = std::move(text);
      body->push_back(n);
      text.clear();
    }
  };
  while (i < src.size()) {
    size_t open = src.find('{', i);
    if (open == string_view::npos || open == src.size() - 1) {
      text.append(src.substr(i));
      break;
    }
    char next = src[open + 1];
    if (next != '{' && next != '%') {
      text.append(src.substr(i, open + 1 - i));
      i = open + 1;
      continue;
    }
    text.append(src.substr(i, open - i));
    if (next == '{') {
      bool raw = open + 2 < src.size() && src[open + 2] == '{';
      size_t close = src.find(raw ? "}}}" : "}}", open + (raw ? 3 : 2));
      if (close == string_view::npos) {
        string msg = "template \"" + name + "\": a ";
        msg += raw ? "{{{" : "{{";
        msg += " is not closed";
        throw TemplateError(msg);
      }
      flushText();
      TNode* n = pool.make();
      n->kind = raw ? 2 : 1;
      size_t a = open + (raw ? 3 : 2);
      string_view p = src.substr(a, close - a);
      string ptrim = trimUnicode(p);
      n->path = pathOf(name, ptrim);
      body->push_back(n);
      i = close + (raw ? 3 : 2);
      continue;
    }
    size_t close = src.find("%}", open + 2);
    if (close == string_view::npos) throw TemplateError("template \"" + name + "\": a {% is not closed");
    size_t end = close + 2;
    size_t lineStart;
    if (open == 0) lineStart = 0;
    else {
      size_t r = src.rfind('\n', open - 1);
      lineStart = (r == string_view::npos) ? 0 : r + 1;
    }
    size_t nl = src.find('\n', end);
    size_t lineEnd = (nl == string_view::npos) ? src.size() : nl;
    bool standalone = allSpaceTab(src.substr(lineStart, open - lineStart)) && allSpaceTab(src.substr(end, lineEnd - end));
    if (standalone) {
      size_t cut = open - lineStart;
      if (cut <= text.size()) text.resize(text.size() - cut);
    }
    flushText();
    string tag = trimUnicode(src.substr(open + 2, close - (open + 2)));
    i = standalone ? (nl == string_view::npos ? src.size() : nl + 1) : end;

    string a, b;
    bool notFlag;
    if (tag == "else") {
      TNode* top = stack.empty() ? nullptr : stack.back();
      if (top == nullptr || top->kind != 3 || top->hasElse) throw TemplateError("template \"" + name + "\": an else outside an if");
      top->hasElse = true;
      body = &top->els;
      continue;
    }
    if (tag == "endfor" || tag == "endif") {
      int want = tag == "endfor" ? 4 : 3;
      TNode* top = stack.empty() ? nullptr : stack.back();
      if (top == nullptr || top->kind != want) throw TemplateError("template \"" + name + "\": an " + tag + " with no open " + tag.substr(3));
      stack.pop_back();
      TNode* parent = stack.empty() ? nullptr : stack.back();
      if (parent == nullptr) body = &root;
      else if (parent->kind == 4) body = &parent->body;
      else body = parent->hasElse ? &parent->els : &parent->body;
      continue;
    }
    TNode* n = pool.make();
    if (matchFor(tag, a, b)) {
      n->kind = 4;
      n->text = a;
      n->path = pathOf(name, b);
      body->push_back(n);
      stack.push_back(n);
      body = &n->body;
      continue;
    }
    if (matchIf(tag, notFlag, a)) {
      n->kind = 3;
      n->notFlag = notFlag;
      n->path = pathOf(name, a);
      body->push_back(n);
      stack.push_back(n);
      body = &n->body;
      continue;
    }
    if (matchInclude(tag, a)) {
      n->kind = 5;
      n->text = a;
      body->push_back(n);
      continue;
    }
    throw TemplateError("template \"" + name + "\": {% " + tag + " %} is not a tag");
  }
  flushText();
  if (!stack.empty()) {
    const char* kind = stack.back()->kind == 4 ? "for" : "if";
    throw TemplateError("template \"" + name + "\": a " + kind + " is not closed");
  }
  return root;
}

// ------------------------------------------------------------------ build model

struct TagRef {
  int tag = -1;          // index into tags, or -1 when no post has this slug
  string name;
  string slug;
  string url;
};

struct TocEntry {
  int level = 0;
  string id;
  string html;
};

struct Source {
  string path;
  string textOwned;
  string_view text;
  string_view body;
  bool post = false;
  bool draft = false;
  bool hasDraft = false;
  bool hasDate = false, hasTitle = false, hasTags = false, hasLayout = false, hasSlug = false, hasSummary = false;
  string title, date, layout, slug, summary;
  vector<string> tags;
  string outDir, url;
  string html, excerpt;
  vector<TocEntry> toc;
  int newer = -1, older = -1;
  vector<TagRef> refs;
  string dateLong;
  int reading = 1;
  Val* card = nullptr;
  Val* page = nullptr;
  Val* link = nullptr;
  const vector<TNode*>* layoutBody = nullptr;
};

struct Tag {
  string name, slug, url;
  vector<int> posts;
  int count = 0;
  Val* val = nullptr;
};

struct StaticFile {
  string_view path, content;
};

struct OutEntry {
  string path;
  string text;
};

struct LinkEnv {
  const string* base;
  const unordered_map<string, int>* bySource;
  const StringSet* statics;
  const vector<Source>* sources;
  unordered_set<string>* broken;
  const string* currentSrc;
  const string* currentDir;
  unordered_map<string, unordered_map<string_view, string>>* cache;
  unordered_map<string_view, string>* currentCache;
};

// ---------------------------------------------------------------- front matter

struct DataError {
  bool error = false;
  string message;
};

static int daysIn(int y, int m) {
  if (m == 2) return ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 29 : 28;
  return (m == 4 || m == 6 || m == 9 || m == 11) ? 30 : 31;
}

static bool allDigits(string_view s) {
  for (char c : s) if (c < '0' || c > '9') return false;
  return true;
}

static DataError stringOf(string_view raw, string& out) {
  DataError e;
  if (!raw.empty() && raw[0] == '"') {
    if (raw.size() < 2 || raw.back() != '"') {
      e.error = true;
      e.message = "a quoted string is not closed";
      return e;
    }
    for (size_t i = 1; i + 1 < raw.size(); i++) {
      char c = raw[i];
      if (c == '\\') {
        if (i + 1 >= raw.size() - 1) {
          e.error = true;
          e.message = "a quoted string holds an escape other than \\\" or \\\\";
          return e;
        }
        char n = raw[i + 1];
        if (n != '"' && n != '\\') {
          e.error = true;
          e.message = "a quoted string holds an escape other than \\\" or \\\\";
          return e;
        }
        out.push_back(n);
        i++;
      } else if (c == '"') {
        e.error = true;
        e.message = "a quoted string holds a stray quote";
        return e;
      } else out.push_back(c);
    }
    return e;
  }
  if (!raw.empty() && raw[0] == '[') {
    e.error = true;
    e.message = "a bare string opens on [";
    return e;
  }
  out.assign(raw);
  return e;
}

static string trimUnicode(string_view s) {
  size_t a = 0, b = s.size();
  while (a < b) {
    size_t j = a;
    uint32_t cp = utf8Next(s, j);
    if (!isJsSpaceCp(cp)) break;
    a = j;
  }
  while (b > a) {
    // find the start of the last code point
    size_t k = b - 1;
    while (k > a && ((unsigned char)s[k] & 0xC0) == 0x80) k--;
    size_t j = k;
    uint32_t cp = utf8Next(s, j);
    if (!isJsSpaceCp(cp)) break;
    b = k;
  }
  return string(s.substr(a, b - a));
}

static DataError parseFrontMatter(Source& s) {
  DataError err;
  string_view text = s.text;
  size_t firstEnd = text.find('\n');
  if (firstEnd == string_view::npos) {
    if (text == "---") { err.error = true; err.message = "the front matter is not closed"; return err; }
    err.error = true; err.message = "no front matter";
    return err;
  }
  if (text.substr(0, firstEnd) != "---") {
    err.error = true; err.message = "no front matter";
    return err;
  }
  size_t closeStart = string_view::npos, closeEnd = string_view::npos;
  size_t pos = firstEnd + 1;
  while (pos <= text.size()) {
    size_t nl = text.find('\n', pos);
    bool last = nl == string_view::npos;
    size_t end = last ? text.size() : nl;
    if (end - pos == 3 && text[pos] == '-' && text[pos + 1] == '-' && text[pos + 2] == '-') {
      closeStart = pos;
      closeEnd = end;
      break;
    }
    if (last) break;
    pos = end + 1;
  }
  if (closeStart == string_view::npos) {
    err.error = true;
    err.message = "the front matter is not closed";
    return err;
  }
  s.body = text.substr(closeEnd == text.size() ? text.size() : closeEnd + 1);
  int lineNo = 1;
  size_t i = firstEnd + 1;
  while (i < closeStart) {
    lineNo++;
    size_t nl = text.find('\n', i);
    size_t end = (nl == string_view::npos || nl > closeStart) ? closeStart : nl;
    string_view line = text.substr(i, end - i);
    bool blank = allSpaceTab(line);
    if (!blank && line[0] != '#') {
      size_t colon = line.find(':');
      bool valid = colon != string_view::npos && colon > 0;
      if (valid) {
        for (size_t k = 0; k < colon; k++) {
          char c = line[k];
          if (!((c >= 'a' && c <= 'z') || c == '_')) { valid = false; break; }
        }
      }
      if (valid && colon + 1 < line.size() && line[colon + 1] != ' ') valid = false;
      if (!valid) {
        err.error = true;
        err.message = "line " + std::to_string(lineNo) + " is not \"key: value\"";
        return err;
      }
      string key(line.substr(0, colon));
      string_view raw = colon + 1 >= line.size() ? string_view() : line.substr(colon + 2);
      size_t e = raw.size();
      while (e > 0 && (raw[e - 1] == ' ' || raw[e - 1] == '\t')) e--;
      raw = raw.substr(0, e);
      bool known = key == "title" || key == "date" || key == "tags" || key == "draft" || key == "layout" || key == "slug" || key == "summary";
      if (!known) {
        err.error = true;
        err.message = "unknown key \"" + key + "\"";
        return err;
      }
      bool seen = (key == "title" && s.hasTitle) || (key == "date" && s.hasDate) || (key == "tags" && s.hasTags) ||
                  (key == "draft" && s.hasDraft) || (key == "layout" && s.hasLayout) || (key == "slug" && s.hasSlug) ||
                  (key == "summary" && s.hasSummary);
      if (seen) {
        err.error = true;
        err.message = "key \"" + key + "\" is given twice";
        return err;
      }
      if (raw.empty()) {
        err.error = true;
        err.message = "key \"" + key + "\" has no value";
        return err;
      }
      if (key == "title" || key == "summary") {
        string v;
        DataError e = stringOf(raw, v);
        if (e.error) {
          err.error = true;
          err.message = "key \"" + key + "\": " + e.message;
          return err;
        }
        if (key == "title") { s.title = std::move(v); s.hasTitle = true; }
        else { s.summary = std::move(v); s.hasSummary = true; }
      } else if (key == "date") {
        bool ok = raw.size() == 10 && raw[4] == '-' && raw[7] == '-' && allDigits(raw.substr(0, 4)) && allDigits(raw.substr(5, 2)) && allDigits(raw.substr(8, 2));
        int y = ok ? std::stoi(string(raw.substr(0, 4))) : 0;
        int mo = ok ? std::stoi(string(raw.substr(5, 2))) : 0;
        int d = ok ? std::stoi(string(raw.substr(8, 2))) : 0;
        if (!ok || y < 1970 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > daysIn(y, mo)) {
          err.error = true;
          err.message = "key \"date\": not a date from 1970-01-01 to 2099-12-31 (YYYY-MM-DD)";
          return err;
        }
        s.date.assign(raw);
        s.hasDate = true;
      } else if (key == "tags") {
        if (raw[0] != '[' || raw.back() != ']') {
          err.error = true;
          err.message = "key \"tags\": not a list [a, b]";
          return err;
        }
        string_view inner = raw.substr(1, raw.size() - 2);
        if (trimUnicode(inner).empty()) {
          s.hasTags = true;
        } else {
          vector<string> tags;
          size_t start = 0;
          bool bad = false;
          while (true) {
            size_t comma = inner.find(',', start);
            string_view part = comma == string_view::npos ? inner.substr(start) : inner.substr(start, comma - start);
            string t = trimUnicode(part);
            if (t.empty()) { bad = true; break; }
            if (t.find_first_of("\"[]") != string::npos) { bad = true; break; }
            tags.push_back(std::move(t));
            if (comma == string_view::npos) break;
            start = comma + 1;
          }
          if (bad) {
            err.error = true;
            err.message = "key \"tags\": an empty tag, or one holding \" [ or ]";
            return err;
          }
          s.tags = std::move(tags);
          s.hasTags = true;
        }
      } else if (key == "draft") {
        if (raw == "true") { s.draft = true; }
        else if (raw == "false") { s.draft = false; }
        else {
          err.error = true;
          err.message = "key \"draft\": true or false";
          return err;
        }
        s.hasDraft = true;
      } else if (key == "layout") {
        bool ok = !raw.empty();
        for (char c : raw) if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) { ok = false; break; }
        if (!ok) {
          err.error = true;
          err.message = "key \"layout\": not a template name";
          return err;
        }
        s.layout.assign(raw);
        s.hasLayout = true;
      } else if (key == "slug") {
        bool ok = !raw.empty() && raw.size() <= 64;
        bool prevDash = true;
        for (char c : raw) {
          if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) prevDash = false;
          else if (c == '-') {
            if (prevDash) { ok = false; break; }
            prevDash = true;
          } else { ok = false; break; }
        }
        if (prevDash) ok = false;
        if (!ok) {
          err.error = true;
          err.message = "key \"slug\": not a slug";
          return err;
        }
        s.slug.assign(raw);
        s.hasSlug = true;
      }
    }
    if (end == closeStart) break;
    i = end + 1;
  }
  if (!s.hasTitle) {
    err.error = true;
    err.message = "no title";
    return err;
  }
  return err;
}

// ---------------------------------------------------------------- markdown

struct MarkdownResult {
  string html;
  vector<TocEntry> toc;
  string excerpt;
  bool hasExcerpt = false;
};

static bool isBlankLine(string_view line) {
  for (char c : line) if (c != ' ' && c != '\t') return false;
  return true;
}

static string_view trimLine(string_view line) {
  size_t a = 0, b = line.size();
  while (a < b && (line[a] == ' ' || line[a] == '\t')) a++;
  while (b > a && (line[b - 1] == ' ' || line[b - 1] == '\t')) b--;
  return line.substr(a, b - a);
}

struct FenceInfo { bool open = false; string_view lang; };

static FenceInfo fenceOpen(string_view line) {
  FenceInfo f;
  if (line.size() < 3 || line[0] != '`' || line[1] != '`' || line[2] != '`') return f;
  size_t k = 3;
  while (k < line.size()) {
    char c = line[k];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '+' || c == '-') k++;
    else break;
  }
  for (size_t j = k; j < line.size(); j++) if (line[j] != ' ' && line[j] != '\t') return f;
  f.open = true;
  f.lang = line.substr(3, k - 3);
  return f;
}

static bool fenceClose(string_view line) {
  if (line.size() < 3 || line[0] != '`' || line[1] != '`' || line[2] != '`') return false;
  for (size_t i = 3; i < line.size(); i++) if (line[i] != ' ' && line[i] != '\t') return false;
  return true;
}

struct HeadingInfo { bool ok = false; int depth = 0; string_view text; };

static HeadingInfo headingOf(string_view line) {
  HeadingInfo h;
  size_t k = 0;
  while (k < line.size() && line[k] == '#') k++;
  if (k == 0 || k > 6 || k == line.size()) return h;
  if (line[k] != ' ' && line[k] != '\t') return h;
  size_t s = k;
  while (s < line.size() && (line[s] == ' ' || line[s] == '\t')) s++;
  size_t e = line.size();
  while (e > s && (line[e - 1] == ' ' || line[e - 1] == '\t')) e--;
  if (e <= s) return h;
  h.ok = true;
  h.depth = (int)k;
  h.text = line.substr(s, e - s);
  return h;
}

static bool isHr(string_view line) {
  if (line.size() < 3) return false;
  char a = line[0];
  if (a == '-') { if (line[1] != '-' || line[2] != '-') return false; }
  else if (a == '*') { if (line[1] != '*' || line[2] != '*') return false; }
  else return false;
  for (size_t i = 3; i < line.size(); i++) if (line[i] != ' ' && line[i] != '\t') return false;
  return true;
}

static bool bulletOf(string_view line, string_view& text) {
  if (line.size() < 2 || line[1] != ' ') return false;
  if (line[0] != '-' && line[0] != '*') return false;
  text = line.substr(2);
  return true;
}

struct OrderedInfo { bool ok = false; int start = 1; string_view text; };

static OrderedInfo orderedOf(string_view line) {
  OrderedInfo o;
  size_t k = 0;
  while (k < line.size() && line[k] >= '0' && line[k] <= '9') {
    k++;
    if (k > 9) return o;
  }
  if (k == 0 || k + 2 > line.size() || line[k] != '.' || line[k + 1] != ' ') return o;
  o.ok = true;
  o.start = 0;
  for (size_t j = 0; j < k; j++) o.start = o.start * 10 + (line[j] - '0');
  o.text = line.substr(k + 2);
  return o;
}

static bool isContinued(string_view line) {
  size_t k = 0;
  while (k < line.size() && line[k] == ' ') k++;
  if (k < 2 || k >= line.size()) return false;
  unsigned char c = (unsigned char)line[k];
  if (c < 0x80) return !isAsciiSpace((char)c);
  size_t j = k;
  uint32_t cp = utf8Next(line, j);
  return !isJsSpaceCp(cp);
}

static bool endsParagraph(string_view line) {
  if (line.empty()) return true;
  char c = line[0];
  if (c == ' ' || c == '\t') return isBlankLine(line);
  if (c == '`') return fenceOpen(line).open;
  if (c == '#') return headingOf(line).ok;
  if (c == '-' || c == '*') return isHr(line);
  return false;
}

static bool hasWhitespace(string_view s) {
  for (size_t i = 0; i < s.size(); i++) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) { if (isAsciiSpace((char)c)) return true; }
    else {
      size_t j = i;
      uint32_t cp = utf8Next(s, j);
      if (isJsSpaceCp(cp)) return true;
      i = j - 1;
    }
  }
  return false;
}

static bool escapable(unsigned char c) {
  switch (c) {
    case '\\': case '`': case '*': case '_': case '[': case ']': case '(': case ')':
    case '#': case '+': case '-': case '.': case '!': case '>': return true;
    default: return false;
  }
}

static size_t findUnescaped(string_view s, string_view what, size_t from) {
  size_t j = s.find(what, from);
  while (j != string_view::npos) {
    size_t backslashes = 0;
    for (size_t k = j; k > from && s[k - 1] == '\\'; k--) backslashes++;
    if ((backslashes & 1) == 0) return j;
    j = s.find(what, j + 1);
  }
  return string_view::npos;
}

static size_t findSingleStar(string_view s, size_t from) {
  for (size_t j = from; j < s.size(); j++) {
    char c = s[j];
    if (c == '\\' && j + 1 < s.size() && escapable((unsigned char)s[j + 1])) { j++; continue; }
    if (c != '*') continue;
    if (j + 1 < s.size() && s[j + 1] == '*') { j++; continue; }
    return j;
  }
  return string_view::npos;
}

static string resolveLink(LinkEnv& env, string_view target);

static void inlineInto(string& out, string_view s, LinkEnv& env) {
  size_t i = 0;
  while (i < s.size()) {
    size_t start = i;
    const char* p = s.data();
    size_t n = s.size();
#if defined(__SSE2__)
    while (i + 16 <= n) {
      int mask = mdMask16(p + i);
      if (mask == 0) { i += 16; continue; }
      i += (size_t)__builtin_ctz((unsigned)mask);
      break;
    }
#endif
    while (i < n) {
      unsigned char c = (unsigned char)p[i];
      if (c < 0x80 && MD_SPECIAL[c] != 0) break;
      i++;
    }
    if (i > start) out.append(p + start, i - start);
    if (i >= s.size()) break;
    unsigned char c = (unsigned char)s[i];
    if (c == '\\') {
      unsigned char n = i + 1 < s.size() ? (unsigned char)s[i + 1] : 0;
      if (i + 1 < s.size() && escapable(n)) {
        if (n == '>') out += "&gt;";
        else out.push_back((char)n);
        i += 2;
      } else {
        out.push_back('\\');
        i++;
      }
      continue;
    }
    if (c == '`') {
      size_t j = s.find('`', i + 1);
      if (j != string_view::npos && j > i + 1) {
        out += "<code>";
        appendEscapedHtml(out, s.substr(i + 1, j - i - 1));
        out += "</code>";
        i = j + 1;
      } else {
        out.push_back('`');
        i++;
      }
      continue;
    }
    if (c == '*') {
      if (i + 1 < s.size() && s[i + 1] == '*') {
        size_t j = findUnescaped(s, "**", i + 2);
        if (j != string_view::npos && j > i + 2) {
          out += "<strong>";
          inlineInto(out, s.substr(i + 2, j - i - 2), env);
          out += "</strong>";
          i = j + 2;
        } else {
          out += "**";
          i += 2;
        }
        continue;
      }
      size_t j = findSingleStar(s, i + 1);
      if (j != string_view::npos && j > i + 1) {
        out += "<em>";
        inlineInto(out, s.substr(i + 1, j - i - 1), env);
        out += "</em>";
        i = j + 1;
      } else {
        out.push_back('*');
        i++;
      }
      continue;
    }
    if (c == '[') {
      size_t k = findUnescaped(s, "]", i + 1);
      size_t m = string_view::npos;
      if (k != string_view::npos && k + 1 < s.size() && s[k + 1] == '(') m = s.find(')', k + 2);
      string_view target = (m == string_view::npos) ? string_view() : s.substr(k + 2, m - k - 2);
      if (!target.empty() && !hasWhitespace(target)) {
        out += "<a href=\"";
        string href = resolveLink(env, target);
        appendEscapedHtml(out, href);
        out += "\">";
        inlineInto(out, s.substr(i + 1, k - i - 1), env);
        out += "</a>";
        i = m + 1;
        continue;
      }
      out.push_back('[');
      i++;
      continue;
    }
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '"') out += "&quot;";
    else out += "&#39;";
    i++;
  }
}

static string inlineRender(string_view s, LinkEnv& env) {
  string out;
  out.reserve(s.size() + s.size() / 4 + 32);
  inlineInto(out, s, env);
  return out;
}

static MarkdownResult renderMarkdown(string_view src, LinkEnv& env, vector<string_view>& lines) {
  lines.clear();
  size_t start = 0;
  while (true) {
    size_t nl = src.find('\n', start);
    if (nl == string_view::npos) {
      lines.push_back(src.substr(start));
      break;
    }
    lines.push_back(src.substr(start, nl - start));
    start = nl + 1;
  }
  MarkdownResult res;
  res.html.reserve(src.size() * 2 + 256);
  bool firstBlock = true;
  string scratch;
  string itemScratch;
  auto beginBlock = [&]() {
    if (!firstBlock) res.html.push_back('\n');
    firstBlock = false;
  };
  vector<string> ids;
  size_t i = 0;
  while (i < lines.size()) {
    string_view line = lines[i];
    if (isBlankLine(line)) { i++; continue; }
    char first = line[0];
    FenceInfo fence;
    if (first == '`') fence = fenceOpen(line);
    if (fence.open) {
      i++;
      beginBlock();
      res.html += "<pre><code";
      if (!fence.lang.empty()) {
        res.html += " class=\"language-";
        appendEscapedHtml(res.html, fence.lang);
        res.html += "\"";
      }
      res.html.push_back('>');
      while (i < lines.size() && !fenceClose(lines[i])) {
        appendEscapedHtml(res.html, lines[i]);
        res.html.push_back('\n');
        i++;
      }
      i++;
      res.html += "</code></pre>";
      continue;
    }
    HeadingInfo h;
    if (first == '#') h = headingOf(line);
    if (h.ok) {
      string slug = slugify(h.text);
      string id = uniqueSlugSmall(slug, ids);
      string html;
      html.reserve(h.text.size() + 16);
      inlineInto(html, h.text, env);
      beginBlock();
      res.html += "<h";
      res.html += std::to_string(h.depth);
      res.html += " id=\"";
      res.html += id;
      res.html += "\">";
      res.html += html;
      res.html += "</h";
      res.html += std::to_string(h.depth);
      res.html += ">";
      if (h.depth == 2 || h.depth == 3) res.toc.push_back({h.depth, id, html});
      i++;
      continue;
    }
    if ((first == '-' || first == '*') && isHr(line)) { beginBlock(); res.html += "<hr>"; i++; continue; }
    if (first == '>') {
      scratch.clear();
      bool first = true;
      while (i < lines.size() && !lines[i].empty() && lines[i][0] == '>') {
        string_view l = lines[i++];
        size_t off = (l.size() > 1 && l[1] == ' ') ? 2 : 1;
        string_view text = trimLine(l.substr(off));
        if (!text.empty()) {
          if (!first) scratch.push_back('\n');
          scratch.append(text);
          first = false;
        }
      }
      beginBlock();
      if (first) res.html += "<blockquote>\n</blockquote>";
      else {
        res.html += "<blockquote>\n<p>";
        inlineInto(res.html, scratch, env);
        res.html += "</p>\n</blockquote>";
      }
      continue;
    }
    string_view bulletText;
    bool isBullet = (first == '-' || first == '*') ? bulletOf(line, bulletText) : false;
    OrderedInfo ordered = (first >= '0' && first <= '9') ? orderedOf(line) : OrderedInfo();
    if (isBullet || ordered.ok) {
      bool isOrdered = ordered.ok;
      beginBlock();
      if (isOrdered && ordered.start != 1) {
        res.html += "<ol start=\"";
        res.html += std::to_string(ordered.start);
        res.html += "\">\n";
      } else res.html += isOrdered ? "<ol>\n" : "<ul>\n";
      itemScratch.clear();
      bool openItem = false;
      auto closeItem = [&]() {
        res.html += "<li>";
        inlineInto(res.html, itemScratch, env);
        res.html += "</li>\n";
        itemScratch.clear();
      };
      while (i < lines.size()) {
        bool isItem = false;
        string_view text;
        if (isOrdered) {
          OrderedInfo m = orderedOf(lines[i]);
          if (m.ok) { isItem = true; text = m.text; }
        } else {
          string_view m;
          if (bulletOf(lines[i], m)) { isItem = true; text = m; }
        }
        if (isItem) {
          if (openItem) closeItem();
          itemScratch.assign(trimLine(text));
          openItem = true;
          i++;
          continue;
        }
        if (openItem && isContinued(lines[i])) {
          itemScratch.push_back('\n');
          itemScratch.append(trimLine(lines[i]));
          i++;
          continue;
        }
        break;
      }
      if (openItem) closeItem();
      res.html += isOrdered ? "</ol>" : "</ul>";
      continue;
    }
    scratch.assign(trimLine(line));
    i++;
    while (i < lines.size() && !endsParagraph(lines[i])) {
      scratch.push_back('\n');
      scratch.append(trimLine(lines[i]));
      i++;
    }
    beginBlock();
    if (!res.hasExcerpt) {
      string html;
      inlineInto(html, scratch, env);
      res.excerpt = html;
      res.hasExcerpt = true;
      res.html += "<p>";
      res.html += html;
      res.html += "</p>";
    } else {
      res.html += "<p>";
      inlineInto(res.html, scratch, env);
      res.html += "</p>";
    }
  }
  if (!firstBlock) res.html.push_back('\n');
  return res;
}

// ---------------------------------------------------------------- link resolution

static string dirOf(string_view path) {
  size_t at = path.rfind('/');
  return at == string_view::npos ? string() : string(path.substr(0, at));
}

static bool hasScheme(string_view target) {
  if (target.empty()) return false;
  char c = target[0];
  if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) return false;
  for (size_t i = 1; i < target.size(); i++) {
    c = target[i];
    if (c == ':') return true;
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '.' || c == '-')) return false;
  }
  return false;
}

static bool resolvePath(string_view dir, string_view rel, string& out) {
  vector<string> parts;
  if (!dir.empty()) {
    size_t start = 0;
    while (true) {
      size_t end = dir.find('/', start);
      if (end == string_view::npos) { parts.emplace_back(dir.substr(start)); break; }
      parts.emplace_back(dir.substr(start, end - start));
      start = end + 1;
    }
  }
  size_t start = 0;
  while (start <= rel.size()) {
    size_t end = rel.find('/', start);
    if (end == string_view::npos) end = rel.size();
    size_t len = end - start;
    if (len == 0) {
      // skip
    } else if (len == 1 && rel[start] == '.') {
      // skip
    } else if (len == 2 && rel[start] == '.' && rel[start + 1] == '.') {
      if (parts.empty()) return false;
      parts.pop_back();
    } else parts.emplace_back(rel.substr(start, len));
    if (end == rel.size()) break;
    start = end + 1;
  }
  out.clear();
  for (size_t i = 0; i < parts.size(); i++) {
    if (i != 0) out.push_back('/');
    out += parts[i];
  }
  return true;
}

static string resolveLink(LinkEnv& env, string_view target) {
  if (hasScheme(target) || target[0] == '/' || target[0] == '#') return string(target);
  auto& dirCache = *env.currentCache;
  auto hit = dirCache.find(target);
  if (hit != dirCache.end()) {
    if (hit->second.size() == 1 && hit->second[0] == '\0') {
      env.broken->insert("broken-link " + *env.currentSrc + " -> " + string(target));
      return string(target);
    }
    return hit->second;
  }
  size_t hash = target.find('#');
  string_view rel = hash == string_view::npos ? target : target.substr(0, hash);
  string_view frag = hash == string_view::npos ? string_view() : target.substr(hash);
  string result;
  if (!rel.empty()) {
    string path;
    if (resolvePath(*env.currentDir, rel, path)) {
      auto it = env.bySource->find(path);
      if (it != env.bySource->end()) result = (*env.sources)[it->second].url + string(frag);
      else if (env.statics->count(path) != 0) result = *env.base + "/" + path + string(frag);
    }
  }
  if (result.empty()) {
    dirCache.emplace(target, string(1, '\0'));
    env.broken->insert("broken-link " + *env.currentSrc + " -> " + string(target));
    return string(target);
  }
  dirCache.emplace(target, result);
  return result;
}

// ---------------------------------------------------------------- rendering

struct Scope {
  bool isLoop = false;
  vector<std::pair<string_view, Val*>> vars;
  Val* loop = nullptr;
  string_view name;
  Val* value = nullptr;
  Scope* parent = nullptr;
};

static Val* lookup(Scope* scope, const vector<string>& path) {
  Val* v = nullptr;
  const string& first = path[0];
  for (Scope* s = scope; s != nullptr; s = s->parent) {
    if (!s->isLoop) {
      bool found = false;
      for (auto& kv : s->vars) {
        if (kv.first == first) { v = kv.second; found = true; break; }
      }
      if (found) break;
    } else {
      if (first == "loop" && s->loop != nullptr) { v = s->loop; break; }
      if (s->name == first) { v = s->value; break; }
    }
  }
  if (v == nullptr) return nullptr;
  for (size_t k = 1; k < path.size(); k++) {
    if (v->t != Val::OBJ) return nullptr;
    Val* next = nullptr;
    for (auto& kv : v->obj) if (kv.first == path[k]) { next = kv.second; break; }
    if (next == nullptr) return nullptr;
    v = next;
  }
  return v;
}

static Val* lookupName(Scope* scope, string_view first) {
  for (Scope* s = scope; s != nullptr; s = s->parent) {
    if (!s->isLoop) {
      for (auto& kv : s->vars) if (kv.first == first) return kv.second;
    } else {
      if (first == "loop" && s->loop != nullptr) return s->loop;
      if (s->name == first) return s->value;
    }
  }
  return nullptr;
}

class Renderer {
 public:
  Buf out;
  Scope* constScope = nullptr;

  void renderNodes(const vector<TNode*>& nodes, Scope* scope, int depth) {
    for (TNode* n : nodes) {
      switch (n->kind) {
        case 0: out.append(n->text); break;
        case 1: {
          Val* v = lookup(scope, n->path);
          if (v != nullptr && v->t == Val::STR) appendEscapedHtml(out, v->s);
          else out += escapeHtml(textOfTemp(v));
          break;
        }
        case 2: textOf(out, lookup(scope, n->path)); break;
        case 3: {
          bool t = truthy(lookup(scope, n->path));
          if (t != n->notFlag) renderNodes(n->body, scope, depth);
          else if (n->hasElse) renderNodes(n->els, scope, depth);
          break;
        }
        case 4: {
          Val* xs = lookup(scope, n->path);
          if (xs == nullptr || xs->t != Val::ARR) break;
          size_t len = xs->arr.size();
          if (n->loopVal == nullptr) {
            n->loopVal = valObj(loopPool);
            n->loopIndex = valNum(loopPool, 1);
            n->loopFirst = valBool(loopPool, true);
            n->loopLast = valBool(loopPool, false);
            n->loopLength = valNum(loopPool, 0);
            objPut(n->loopVal, "index", n->loopIndex);
            objPut(n->loopVal, "first", n->loopFirst);
            objPut(n->loopVal, "last", n->loopLast);
            objPut(n->loopVal, "length", n->loopLength);
          }
          n->loopLength->n = (double)len;
          for (size_t k = 0; k < len; k++) {
            n->loopIndex->n = (double)(k + 1);
            n->loopFirst->b = k == 0;
            n->loopLast->b = k == len - 1;
            Scope child;
            child.isLoop = true;
            child.name = n->text;
            child.value = xs->arr[k];
            child.loop = n->loopVal;
            child.parent = scope;
            renderNodes(n->body, &child, depth);
          }
          break;
        }
        case 5: {
          if (n->includeNodes == nullptr || depth >= 8) break;
          if (n->constant && depth + n->includeDepth <= 8) {
            if (!n->constReady) {
              Buf saved;
              saved.swap(out);
              renderNodes(*n->includeNodes, constScope, depth + 1);
              Buf value;
              value.swap(out);
              out.swap(saved);
              n->constHtml = value.str();
              n->constReady = true;
            }
            out += n->constHtml;
          } else if (!n->singleVar.empty() && depth + n->includeDepth <= 8) {
            Val* key = lookupName(scope, n->singleVar);
            if (key != nullptr) {
              auto it = n->valCache.find(key);
              if (it == n->valCache.end()) {
                Buf saved;
                saved.swap(out);
                renderNodes(*n->includeNodes, scope, depth + 1);
                Buf value;
                value.swap(out);
                out.swap(saved);
                it = n->valCache.emplace(key, value.str()).first;
              }
              out += it->second;
            } else {
              renderNodes(*n->includeNodes, scope, depth + 1);
            }
          } else {
            renderNodes(*n->includeNodes, scope, depth + 1);
          }
          break;
        }
      }
    }
  }

 private:
  ValPool loopPool;
  ValPool numPool;

  string textOfTemp(const Val* v) {
    string s;
    textOf(s, v);
    return s;
  }
};

// ---------------------------------------------------------------- build

struct Build {
  const CaseInput* input;
  string base;
  string siteTitle;
  int perPage = 0, recent = 0;

  vector<Template> templates;
  unordered_map<string, int> templateIndex;
  TemplatePool tpool;
  vector<Source> sources;
  vector<StaticFile> statics;
  unordered_map<string, int> bySource;
  StringSet staticPaths;
  unordered_set<string> broken;
  vector<string> report;
  vector<int> posts;
  vector<Tag> tags;
  vector<int> tagOrder;

  vector<OutEntry> output;
  unordered_map<string, int> outIndex;

  ValPool vals;
  Val* siteVal = nullptr;
  Val* tagsVal = nullptr;
  Val* recentVal = nullptr;
  vector<Val*> cards;
  vector<Val*> pages;

  void setOutput(string path, string text) {
    auto it = outIndex.find(path);
    if (it != outIndex.end()) output[it->second].text = std::move(text);
    else {
      outIndex.emplace(path, output.size());
      output.push_back({std::move(path), std::move(text)});
    }
  }

  void parseTemplates() {
    auto list = input->templates;
    std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& kv : list) {
      string name(kv.first);
      Template tmpl;
      tmpl.name = name;
      try {
        tmpl.body = parseTemplate(name, kv.second, tpool);
        tmpl.ok = true;
      } catch (const TemplateError& e) {
        tmpl.error = e.what();
        report.push_back(tmpl.error);
      }
      templateIndex[name] = (int)templates.size();
      templates.push_back(std::move(tmpl));
    }
  }
  void readSources() {
    auto fs = input->files;
    std::sort(fs.begin(), fs.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& f : fs) {
      string_view path = f.first;
      string_view content = f.second;
      if (path.size() >= 3 && path.compare(path.size() - 3, 3, ".md") == 0) {
        Source s;
        s.path.assign(path);
        if (content.find('\r') == string_view::npos) s.text = content;
        else {
          s.textOwned.reserve(content.size());
          for (size_t i = 0; i < content.size(); i++) {
            if (content[i] == '\r') {
              if (i + 1 < content.size() && content[i + 1] == '\n') i++;
              s.textOwned.push_back('\n');
            } else s.textOwned.push_back(content[i]);
          }
          s.text = s.textOwned;
        }
        sources.push_back(std::move(s));
      } else {
        statics.push_back({path, content});
      }
    }
  }
  void frontMatterPass() {
    vector<Source> kept;
    for (Source& s : sources) {
      DataError e = parseFrontMatter(s);
      if (e.error) {
        report.push_back("refused " + s.path + ": " + e.message);
        continue;
      }
      s.post = s.path.rfind("posts/", 0) == 0;
      if (s.path.rfind("tags/", 0) == 0) {
        report.push_back("refused " + s.path + ": tags/ is the generator's own");
        continue;
      }
      if (s.path.rfind("page/", 0) == 0) {
        report.push_back("refused " + s.path + ": page/ is the generator's own");
        continue;
      }
      if (s.post && !s.hasDate) {
        report.push_back("refused " + s.path + ": a post needs a date");
        continue;
      }
      string layout = s.hasLayout ? s.layout : (s.post ? "post" : "page");
      auto it = templateIndex.find(layout);
      if (it == templateIndex.end() || !templates[it->second].ok) {
        report.push_back("refused " + s.path + ": layout \"" + layout + "\" is no template that parses");
        continue;
      }
      s.layout = layout;
      s.layoutBody = &templates[it->second].body;
      if (s.draft) {
        report.push_back("draft " + s.path);
        continue;
      }
      kept.push_back(std::move(s));
    }
    sources = std::move(kept);
  }

  void permalinksPass() {
    unordered_map<string, StringSet> taken;
    for (Source& s : sources) {
      string dir = s.post ? string("posts") : dirOf(s.path);
      auto& set = taken[dir];
      if (set.empty() && dir.empty()) {
        set.insert("posts"); set.insert("tags"); set.insert("page");
      }
      string baseSlug = s.hasSlug ? s.slug : slugify(s.title);
      string slug = uniqueSlug(baseSlug, set);
      s.slug = slug;
      s.outDir = dir.empty() ? slug : dir + "/" + slug;
      s.url = base + "/" + s.outDir + "/";
      bySource[s.path] = (int)(&s - sources.data());
    }
  }

  void markdownPass() {
    for (auto& f : statics) staticPaths.emplace(f.path);
    LinkEnv env;
    vector<string_view> lineScratch;
    env.base = &base;
    env.bySource = &bySource;
    env.statics = &staticPaths;
    env.sources = &sources;
    env.broken = &broken;
    unordered_map<string, unordered_map<string_view, string>> linkCache;
    string curDir, curSrc;
    env.currentDir = &curDir;
    env.currentSrc = &curSrc;
    env.cache = &linkCache;
    for (Source& s : sources) {
      curDir = dirOf(s.path);
      curSrc = s.path;
      auto it = linkCache.find(curDir);
      if (it == linkCache.end()) it = linkCache.emplace(curDir, unordered_map<string_view, string>()).first;
      env.currentCache = &it->second;
      MarkdownResult md = renderMarkdown(s.body, env, lineScratch);
      s.html = std::move(md.html);
      s.toc = std::move(md.toc);
      s.excerpt = std::move(md.excerpt);
      s.reading = std::max(1, (int)std::ceil(countWords(s.body) / 200.0));
      if (s.hasDate) s.dateLong = longDate(s.date);
    }
  }

  void collectionsPass() {
    for (size_t i = 0; i < sources.size(); i++) if (sources[i].post) posts.push_back((int)i);
    std::sort(posts.begin(), posts.end(), [&](int a, int b) {
      const Source& x = sources[a];
      const Source& y = sources[b];
      if (x.date != y.date) return x.date > y.date;
      return x.slug < y.slug;
    });
    for (size_t k = 0; k < posts.size(); k++) {
      Source& p = sources[posts[k]];
      p.newer = k == 0 ? -1 : posts[k - 1];
      p.older = k + 1 == posts.size() ? -1 : posts[k + 1];
    }
  }

  vector<string> ownTags(const Source& s) {
    vector<string> out;
    vector<string> seen;
    for (const string& name : s.tags) {
      string slug = slugify(name);
      bool dup = false;
      for (const string& x : seen) if (x == slug) { dup = true; break; }
      if (dup) continue;
      seen.push_back(slug);
      out.push_back(name);
    }
    return out;
  }

  void tagsPass() {
    unordered_map<string, int> bySlug;
    for (int pi : posts) {
      Source& p = sources[pi];
      vector<string> names = ownTags(p);
      for (const string& name : names) {
        string slug = slugify(name);
        auto it = bySlug.find(slug);
        int idx;
        if (it == bySlug.end()) {
          idx = (int)tags.size();
          Tag t;
          t.name = name;
          t.slug = slug;
          t.url = base + "/tags/" + slug + "/";
          tags.push_back(std::move(t));
          bySlug[slug] = idx;
        } else idx = it->second;
        tags[idx].posts.push_back(pi);
        TagRef ref;
        ref.tag = idx;
        ref.name = tags[idx].name;
        ref.slug = tags[idx].slug;
        ref.url = tags[idx].url;
        p.refs.push_back(std::move(ref));
      }
    }
    for (Source& s : sources) {
      if (s.post) continue;
      vector<string> names = ownTags(s);
      for (const string& name : names) {
        string slug = slugify(name);
        TagRef ref;
        auto it = bySlug.find(slug);
        if (it != bySlug.end()) {
          int idx = it->second;
          ref.tag = idx;
          ref.name = tags[idx].name;
          ref.slug = tags[idx].slug;
          ref.url = tags[idx].url;
        } else {
          ref.tag = -1;
          ref.name = name;
          ref.slug = slug;
          ref.url.clear();
        }
        s.refs.push_back(std::move(ref));
      }
    }
    for (size_t i = 0; i < tags.size(); i++) {
      tags[i].count = (int)tags[i].posts.size();
      tagOrder.push_back((int)i);
    }
    std::sort(tagOrder.begin(), tagOrder.end(), [&](int a, int b) {
      if (tags[a].count != tags[b].count) return tags[a].count > tags[b].count;
      return tags[a].slug < tags[b].slug;
    });
  }

  Val* makeTagVal(const TagRef& r, Val* postsArr = nullptr) {
    Val* v = valObj(vals);
    objPut(v, "name", valStrV(vals, r.name));
    objPut(v, "slug", valStrV(vals, r.slug));
    objPut(v, "url", valStrV(vals, r.url));
    objPut(v, "posts", postsArr != nullptr ? postsArr : valArr(vals));
    objPut(v, "count", valNum(vals, 0));
    return v;
  }

  void valuesPass() {
    // Tag values first, so cards can point at them.
    for (Tag& t : tags) {
      Val* v = valObj(vals);
      objPut(v, "name", valStrV(vals, t.name));
      objPut(v, "slug", valStrV(vals, t.slug));
      objPut(v, "url", valStrV(vals, t.url));
      objPut(v, "posts", valArr(vals));
      objPut(v, "count", valNum(vals, (double)t.count));
      t.val = v;
    }
    // Cards for every source.
    cards.resize(sources.size(), nullptr);
    for (size_t i = 0; i < sources.size(); i++) {
      Source& s = sources[i];
      Val* card = valObj(vals);
      objPut(card, "title", valStrV(vals, s.title));
      objPut(card, "url", valStrV(vals, s.url));
      objPut(card, "date", valStrV(vals, s.hasDate ? string_view(s.date) : string_view()));
      objPut(card, "date_long", valStrV(vals, s.hasDate ? string_view(s.dateLong) : string_view()));
      objPut(card, "summary", valStrV(vals, s.hasSummary ? string_view(s.summary) : string_view()));
      objPut(card, "excerpt", valStrV(vals, s.excerpt));
      objPut(card, "reading_minutes", valNum(vals, s.reading));
      Val* tagArr = valArr(vals);
      for (const TagRef& r : s.refs) {
        if (r.tag >= 0) tagArr->arr.push_back(tags[r.tag].val);
        else tagArr->arr.push_back(makeTagVal(r));
      }
      objPut(card, "tags", tagArr);
      s.card = card;
      cards[i] = card;
    }
    // Fill the tags' post arrays with the cards.
    for (Tag& t : tags) {
      Val* arr = nullptr;
      for (auto& kv : t.val->obj) if (kv.first == "posts") { arr = kv.second; break; }
      arr->arr.reserve(t.posts.size());
      for (int pi : t.posts) arr->arr.push_back(cards[pi]);
    }
    // Link values and page values.
    pages.resize(sources.size(), nullptr);
    for (size_t i = 0; i < sources.size(); i++) {
      Source& s = sources[i];
      if (s.newer >= 0) {
        Source& n = sources[s.newer];
        Val* l = valObj(vals);
        objPut(l, "title", valStrV(vals, n.title));
        objPut(l, "url", valStrV(vals, n.url));
        objPut(l, "date", valStrV(vals, n.hasDate ? string_view(n.date) : string_view()));
        objPut(l, "date_long", valStrV(vals, n.hasDate ? string_view(n.dateLong) : string_view()));
        s.link = l;
      }
      Val* page = valObj(vals);
      objPut(page, "title", valStrV(vals, s.title));
      objPut(page, "url", valStrV(vals, s.url));
      objPut(page, "date", valStrV(vals, s.hasDate ? string_view(s.date) : string_view()));
      objPut(page, "date_long", valStrV(vals, s.hasDate ? string_view(s.dateLong) : string_view()));
      objPut(page, "summary", valStrV(vals, s.hasSummary ? string_view(s.summary) : string_view()));
      objPut(page, "excerpt", valStrV(vals, s.excerpt));
      objPut(page, "reading_minutes", valNum(vals, s.reading));
      Val* tagArr = valArr(vals);
      for (const TagRef& r : s.refs) {
        if (r.tag >= 0) tagArr->arr.push_back(tags[r.tag].val);
        else tagArr->arr.push_back(makeTagVal(r));
      }
      objPut(page, "tags", tagArr);
      objPut(page, "slug", valStrV(vals, s.slug));
      objPut(page, "content", valStrV(vals, s.html));
      Val* toc = valArr(vals);
      for (const TocEntry& e : s.toc) {
        Val* h = valObj(vals);
        objPut(h, "level", valNum(vals, e.level));
        objPut(h, "id", valStrV(vals, e.id));
        objPut(h, "html", valStrV(vals, e.html));
        toc->arr.push_back(h);
      }
      objPut(page, "toc", toc);
      if (s.newer >= 0) {
        // link object was stored on the newer source; make the page's own link values
        Source& n = sources[s.newer];
        Val* l = valObj(vals);
        objPut(l, "title", valStrV(vals, n.title));
        objPut(l, "url", valStrV(vals, n.url));
        objPut(l, "date", valStrV(vals, n.hasDate ? string_view(n.date) : string_view()));
        objPut(l, "date_long", valStrV(vals, n.hasDate ? string_view(n.dateLong) : string_view()));
        objPut(page, "newer", l);
      } else objPut(page, "newer", valNull(vals));
      if (s.older >= 0) {
        Source& n = sources[s.older];
        Val* l = valObj(vals);
        objPut(l, "title", valStrV(vals, n.title));
        objPut(l, "url", valStrV(vals, n.url));
        objPut(l, "date", valStrV(vals, n.hasDate ? string_view(n.date) : string_view()));
        objPut(l, "date_long", valStrV(vals, n.hasDate ? string_view(n.dateLong) : string_view()));
        objPut(page, "older", l);
      } else objPut(page, "older", valNull(vals));
      objPut(page, "is_post", valBool(vals, s.post));
      s.page = page;
      pages[i] = page;
    }

    siteVal = valObj(vals);
    objPut(siteVal, "title", valStrV(vals, siteTitle));
    objPut(siteVal, "base_url", valStrV(vals, base));

    tagsVal = valArr(vals);
    for (int ti : tagOrder) tagsVal->arr.push_back(tags[ti].val);

    recentVal = valArr(vals);
    int nRec = std::min(recent, (int)posts.size());
    for (int k = 0; k < nRec; k++) recentVal->arr.push_back(cards[posts[k]]);
  }

  unordered_map<string, vector<TNode*>> templateMap() {
    unordered_map<string, vector<TNode*>> m;
    for (const Template& t : templates) if (t.ok) m[t.name] = t.body;
    return m;
  }

  void renderPass() {
    Renderer r;
    Scope root;
    root.vars = {{"site", siteVal}, {"tags", tagsVal}, {"recent", recentVal}, {"page", nullptr}, {"posts", nullptr}, {"pager", nullptr}, {"tag", nullptr}};
    Scope constScope;
    constScope.vars = {{"site", siteVal}, {"tags", tagsVal}, {"recent", recentVal}};
    r.constScope = &constScope;

    const vector<TNode*>* indexBody = nullptr;
    const vector<TNode*>* tagBody = nullptr;
    const vector<TNode*>* tagsBody = nullptr;
    auto bodyOf = [&](const char* name) -> const vector<TNode*>* {
      auto it = templateIndex.find(name);
      if (it == templateIndex.end() || !templates[it->second].ok) return nullptr;
      return &templates[it->second].body;
    };
    indexBody = bodyOf("index");
    tagBody = bodyOf("tag");
    tagsBody = bodyOf("tags");

    auto renderBody = [&](const vector<TNode*>* body, Val* page, Val* posts, Val* pager, Val* tag) -> bool {
      if (body == nullptr) return false;
      root.vars[0].second = siteVal;
      root.vars[1].second = tagsVal;
      root.vars[2].second = recentVal;
      root.vars[3].second = page;
      root.vars[4].second = posts;
      root.vars[5].second = pager;
      root.vars[6].second = tag;
      r.out.clear();
      r.out.reserve(32768);
      r.renderNodes(*body, &root, 0);
      return true;
    };

    for (size_t i = 0; i < sources.size(); i++) {
      Source& s = sources[i];
      if (renderBody(s.layoutBody, pages[i], nullptr, nullptr, nullptr)) setOutput(s.outDir + "/index.html", r.out.str());
    }

    int pagesCount = perPage > 0 ? std::max(1, (int)std::ceil((double)posts.size() / perPage)) : 1;
    auto indexUrl = [&](int k) { return k == 1 ? base + "/" : base + "/page/" + std::to_string(k) + "/"; };
    for (int k = 1; k <= pagesCount; k++) {
      Val* arr = valArr(vals);
      int from = (k - 1) * perPage;
      int to = std::min(k * perPage, (int)posts.size());
      if (from < 0) from = 0;
      for (int i = from; i < to; i++) arr->arr.push_back(cards[posts[i]]);
      Val* pager = valObj(vals);
      objPut(pager, "number", valNum(vals, k));
      objPut(pager, "total", valNum(vals, pagesCount));
      objPut(pager, "url", valStr(vals, indexUrl(k)));
      objPut(pager, "newer_url", valStr(vals, k == 1 ? string() : indexUrl(k - 1)));
      objPut(pager, "older_url", valStr(vals, k == pagesCount ? string() : indexUrl(k + 1)));
      if (renderBody(indexBody, nullptr, arr, pager, nullptr)) setOutput(k == 1 ? "index.html" : "page/" + std::to_string(k) + "/index.html", r.out.str());
    }

    for (int ti : tagOrder) {
      Tag& t = tags[ti];
      if (renderBody(tagBody, nullptr, nullptr, nullptr, t.val)) setOutput("tags/" + t.slug + "/index.html", r.out.str());
    }
    if (renderBody(tagsBody, nullptr, nullptr, nullptr, nullptr)) setOutput("tags/index.html", r.out.str());

    for (const StaticFile& f : statics) {
      if (outIndex.count(string(f.path)) != 0) report.push_back("shadowed " + string(f.path) + ": a built page has its path");
      else setOutput(string(f.path), string(f.content));
    }
  }

  void reportPass() {
    vector<string> lines = report;
    lines.insert(lines.end(), broken.begin(), broken.end());
    std::sort(lines.begin(), lines.end());
    int pageCount = 0;
    for (const Source& s : sources) if (!s.post) pageCount++;
    lines.push_back("built " + std::to_string(pageCount) + " pages, " + std::to_string(posts.size()) + " posts, " +
                    std::to_string(tags.size()) + " tags and " + std::to_string(statics.size()) + " static files");
    string text;
    for (const string& l : lines) { text += l; text.push_back('\n'); }
    setOutput("build-report.txt", std::move(text));
  }

  void collectFree(const vector<TNode*>& nodes, unordered_set<string>& bound, unordered_set<string>& free, bool& cyc) {
    for (TNode* n : nodes) {
      if (n->kind == 1 || n->kind == 2) {
        if (n->path.empty()) continue;
        if (bound.count(n->path[0]) == 0) free.insert(n->path[0]);
      } else if (n->kind == 3) {
        if (!n->path.empty() && bound.count(n->path[0]) == 0) free.insert(n->path[0]);
        collectFree(n->body, bound, free, cyc);
        if (n->hasElse) collectFree(n->els, bound, free, cyc);
      } else if (n->kind == 4) {
        if (!n->path.empty() && bound.count(n->path[0]) == 0) free.insert(n->path[0]);
        unordered_set<string> inner = bound;
        inner.insert(n->text);
        inner.insert("loop");
        collectFree(n->body, inner, free, cyc);
      } else if (n->kind == 5) {
        auto it = templateIndex.find(n->text);
        if (it == templateIndex.end()) continue;
        int idx = it->second;
        if (!templates[idx].ok) continue;
        if (templates[idx].state == 1) {
          templates[idx].cycle = true;
          cyc = true;
          free.insert("?");
          continue;
        }
        computeFree(idx);
        Template& t = templates[idx];
        if (t.cycle) { cyc = true; free.insert("?"); }
        for (const string& v : t.freeVars) if (bound.count(v) == 0) free.insert(v);
      }
    }
  }

  void computeFree(int idx) {
    Template& t = templates[idx];
    if (t.state == 2) return;
    if (t.state == 1) { t.cycle = true; return; }
    t.state = 1;
    unordered_set<string> bound, free;
    bool cyc = false;
    collectFree(t.body, bound, free, cyc);
    t.freeVars = std::move(free);
    if (cyc) { t.cycle = true; t.freeVars.insert("?"); }
    t.state = 2;
  }

  int depthOfTemplate(int idx, unordered_set<int>& visiting) {
    if (visiting.count(idx) != 0) return 8;
    visiting.insert(idx);
    int best = 0;
    auto walk = [&](auto&& self, const vector<TNode*>& nodes) -> void {
      for (TNode* n : nodes) {
        if (n->kind == 5) {
          auto it = templateIndex.find(n->text);
          if (it != templateIndex.end() && templates[it->second].ok) {
            int d = 1 + depthOfTemplate(it->second, visiting);
            if (d > best) best = d;
          }
        }
        self(self, n->body);
        if (n->hasElse) self(self, n->els);
      }
    };
    walk(walk, templates[idx].body);
    visiting.erase(idx);
    return best > 8 ? 8 : best;
  }

  void markConstantIncludes(vector<TNode*>& nodes) {
    for (TNode* n : nodes) {
      if (n->kind == 5) {
        auto it = templateIndex.find(n->text);
        if (it != templateIndex.end() && templates[it->second].ok) {
          Template& t = templates[it->second];
          n->includeNodes = &t.body;
          n->includeDepth = 1 + t.maxIncludeDepth;
          if (t.freeConst) n->constant = true;
          else if (t.freeVars.size() == 1) n->singleVar = *t.freeVars.begin();
        }
      }
      markConstantIncludes(n->body);
      if (n->hasElse) markConstantIncludes(n->els);
    }
  }

  void analyzeTemplates() {
    for (size_t i = 0; i < templates.size(); i++) if (templates[i].ok) computeFree((int)i);
    unordered_set<int> visiting;
    for (size_t i = 0; i < templates.size(); i++) {
      if (!templates[i].ok) continue;
      templates[i].maxIncludeDepth = depthOfTemplate((int)i, visiting);
      bool ok = !templates[i].cycle;
      for (const string& v : templates[i].freeVars) {
        if (v != "site" && v != "tags" && v != "recent") { ok = false; break; }
      }
      templates[i].freeConst = ok;
      if (getenv("ENGINE_TIMING")) {
        fprintf(stderr, "  free %s:", templates[i].name.c_str());
        for (const string& v : templates[i].freeVars) fprintf(stderr, " %s", v.c_str());
        fprintf(stderr, " const=%d depth=%d\n", (int)ok, templates[i].maxIncludeDepth);
      }
    }
    for (Template& t : templates) if (t.ok) markConstantIncludes(t.body);
  }

  void run() {
    siteTitle.assign(input->siteTitle);
    base.assign(input->baseUrl);
    while (!base.empty() && base.back() == '/') base.pop_back();
    perPage = input->perPage;
    recent = input->recent;

    bool tm = getenv("ENGINE_TIMING") != nullptr;
    double t;
    t = nowMs(); parseTemplates(); analyzeTemplates(); if (tm) fprintf(stderr, "  templates %.2f\n", nowMs()-t);
    t = nowMs(); readSources(); if (tm) fprintf(stderr, "  sources %.2f\n", nowMs()-t);
    t = nowMs(); frontMatterPass(); if (tm) fprintf(stderr, "  frontmatter %.2f\n", nowMs()-t);
    t = nowMs(); permalinksPass(); if (tm) fprintf(stderr, "  permalinks %.2f\n", nowMs()-t);
    t = nowMs(); markdownPass(); if (tm) fprintf(stderr, "  markdown %.2f\n", nowMs()-t);
    t = nowMs(); collectionsPass(); if (tm) fprintf(stderr, "  collections %.2f\n", nowMs()-t);
    t = nowMs(); tagsPass(); if (tm) fprintf(stderr, "  tags %.2f\n", nowMs()-t);
    t = nowMs(); valuesPass(); if (tm) fprintf(stderr, "  values %.2f\n", nowMs()-t);
    t = nowMs(); renderPass(); if (tm) fprintf(stderr, "  render %.2f\n", nowMs()-t);
    t = nowMs(); reportPass(); if (tm) fprintf(stderr, "  report %.2f\n", nowMs()-t);
  }
};

// ------------------------------------------------------------------ add-on

static double nowMs() {
  using namespace std::chrono;
  return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}
static bool timingOn() { static int v = getenv("ENGINE_TIMING") != nullptr; return v; }

static void appendAnswers(OutBuf& dst, const Build& b) {
  dst.put('{');
  for (size_t i = 0; i < b.output.size(); i++) {
    if (i != 0) dst.put(',');
    jsonStringTo(dst, b.output[i].path);
    dst.put(':');
    jsonStringTo(dst, b.output[i].text);
  }
  dst.put('}');
}

static OutBuf runAll(string_view casesJson) {
  double t0 = nowMs();
  CasesParser parser(casesJson);
  vector<CaseInput> cases = parser.parse();
  if (timingOn()) fprintf(stderr, "parse %.2f\n", nowMs() - t0);
  OutBuf out;
  out.ensure(casesJson.size() * 3 + 64);
  out.put('[');
  for (size_t i = 0; i < cases.size(); i++) {
    if (i != 0) out.put(',');
    Build b;
    b.input = &cases[i];
    double tb = nowMs();
    b.run();
    if (timingOn()) fprintf(stderr, "build %zu %.2f\n", i, nowMs() - tb);
    double ta = nowMs();
    appendAnswers(out, b);
    if (timingOn()) fprintf(stderr, "append %zu %.2f\n", i, nowMs() - ta);
  }
  out.put(']');
  return out;
}

#ifndef NATIVE_TEST
static void finalizeBuffer(napi_env env, void* data, void* hint) {
  free(data);
}

static napi_value RunAll(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 1) {
    napi_throw_type_error(env, nullptr, "runAll needs the cases JSON");
    return nullptr;
  }
  string_view input;
  string owned;
  bool isBuffer = false;
  napi_is_buffer(env, argv[0], &isBuffer);
  if (isBuffer) {
    void* data = nullptr;
    size_t len = 0;
    napi_get_buffer_info(env, argv[0], &data, &len);
    input = string_view((const char*)data, len);
  } else {
    size_t len = 0;
    napi_get_value_string_utf8(env, argv[0], nullptr, 0, &len);
    owned.resize(len + 1);
    napi_get_value_string_utf8(env, argv[0], owned.data(), len + 1, &len);
    owned.resize(len);
    input = owned;
  }
  try {
    OutBuf result = runAll(input);
    char* data = result.data;
    size_t len = result.len;
    result.data = nullptr;
    result.len = result.cap = 0;
    napi_value buf;
    napi_create_external_buffer(env, len, data, finalizeBuffer, nullptr, &buf);
    return buf;
  } catch (const std::exception& e) {
    napi_throw_error(env, nullptr, e.what());
    return nullptr;
  }
}
#endif

#ifndef NATIVE_TEST
static napi_value RunFile(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  if (argc < 2) {
    napi_throw_type_error(env, nullptr, "runFile needs an input and an output path");
    return nullptr;
  }
  auto pathOf = [&](napi_value v, string& out) {
    size_t len = 0;
    napi_get_value_string_utf8(env, v, nullptr, 0, &len);
    out.resize(len + 1);
    napi_get_value_string_utf8(env, v, out.data(), len + 1, &len);
    out.resize(len);
  };
  string inPath, outPath;
  pathOf(argv[0], inPath);
  pathOf(argv[1], outPath);
  try {
    int fd = open(inPath.c_str(), O_RDONLY);
    if (fd < 0) throw std::runtime_error("cannot open " + inPath);
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); throw std::runtime_error("cannot stat " + inPath); }
    size_t size = (size_t)st.st_size;
    string input;
    input.resize(size);
    size_t got = 0;
    while (got < size) {
      ssize_t n = read(fd, input.data() + got, size - got);
      if (n <= 0) { close(fd); throw std::runtime_error("cannot read " + inPath); }
      got += (size_t)n;
    }
    close(fd);

    OutBuf result = runAll(input);

    int out = open(outPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) throw std::runtime_error("cannot open " + outPath);
    size_t wrote = 0;
    while (wrote < result.len) {
      ssize_t n = write(out, result.data + wrote, result.len - wrote);
      if (n <= 0) { close(out); throw std::runtime_error("cannot write " + outPath); }
      wrote += (size_t)n;
    }
    close(out);
  } catch (const std::exception& e) {
    napi_throw_error(env, nullptr, e.what());
    return nullptr;
  }
  return nullptr;
}

#endif

#ifndef NATIVE_TEST
NAPI_MODULE_INIT() {
  napi_value fn;
  napi_create_function(env, "runAll", NAPI_AUTO_LENGTH, RunAll, nullptr, &fn);
  napi_set_named_property(env, exports, "runAll", fn);
  napi_value fn2;
  napi_create_function(env, "runFile", NAPI_AUTO_LENGTH, RunFile, nullptr, &fn2);
  napi_set_named_property(env, exports, "runFile", fn2);
  return exports;
}
#endif
