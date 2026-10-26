#include "runtime/library/regex/regex/regex.h"
#include <stdlib.h>
#include <string.h>

#ifdef TZ_RE_FOR_COMPILER
static void *re_alloc(size_t size) {
  void *p = malloc(size ? size : 1);
  if (!p) {
    abort();
  }

  return p;
}
static void re_dealloc(void *p) { free(p); }
#else
#include "runtime/core/alloc/alloc.h"
#include <stddef.h>
#include <stdint.h>
static void *re_alloc(size_t size) { return tz_alloc(size ? size : 1); }
static void re_dealloc(void *p) { tz_free(p); }
#endif

typedef struct {
  uint32_t cp, next;
} ReOrbit;

typedef struct {
  uint32_t start, value;
} ReRun;

typedef struct {
  uint32_t first, last;
} ReRange;

typedef struct {
  uint32_t offset, count;
} ReList;

enum { RE_NAME_CATEGORY, RE_NAME_SCRIPT, RE_NAME_BINARY };

typedef struct {
  const char *name;
  int kind;
  uint32_t value;
} ReName;

#include "runtime/library/regex/regex_compile/regex_tables.h"

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define MAX_CODE_POINT 0x10FFFFu
#define MAX_DEPTH 500
#define MAX_CAPTURES 65535

typedef struct {
  uint32_t *data;
  size_t length, capacity;
} Vec;

static void vec_reserve(Vec *v, size_t extra) {
  if (v->length + extra <= v->capacity) {
    return;
  }
  size_t capacity = v->capacity * 2 + 16;
  if (capacity < v->length + extra) {
    capacity = v->length + extra;
  }
  uint32_t *data = (uint32_t *)re_alloc(capacity * sizeof(uint32_t));
  if (v->length) {
    memcpy(data, v->data, v->length * sizeof(uint32_t));
  }
  re_dealloc(v->data);
  v->data = data;
  v->capacity = capacity;
}

static void vec_push(Vec *v, uint32_t x) {
  vec_reserve(v, 1);
  v->data[v->length++] = x;
}

static void vec_free(Vec *v) {
  re_dealloc(v->data);
  v->data = NULL;
  v->length = v->capacity = 0;
}

static void vec_copy(Vec *to, const Vec *from) {
  to->length = 0;
  vec_reserve(to, from->length);
  if (from->length) {
    memcpy(to->data, from->data, from->length * sizeof(uint32_t));
  }
  to->length = from->length;
}

typedef struct {
  Vec r;
} CharSet;

static void set_add(CharSet *s, uint32_t first, uint32_t last) {
  vec_push(&s->r, first);
  vec_push(&s->r, last);
}

static size_t set_ranges(const CharSet *s) { return s->r.length / 2; }

static int compare_ranges(const void *a, const void *b) {
  uint32_t x = ((const ReRange *)a)->first, y = ((const ReRange *)b)->first;

  return x < y ? -1 : x > y;
}

static void set_normalize(CharSet *s) {
  size_t n = set_ranges(s);
  if (n < 2) {
    return;
  }
  ReRange *r = (ReRange *)s->r.data;
  qsort(r, n, sizeof(ReRange), compare_ranges);
  size_t out = 0;
  for (size_t i = 1; i < n; ++i) {
    if (r[i].first <= r[out].last + 1 && r[out].last != 0xFFFFFFFFu) {
      if (r[i].last > r[out].last) {
        r[out].last = r[i].last;
      }
    } else {
      r[++out] = r[i];
    }
  }
  s->r.length = (out + 1) * 2;
}

static void set_copy(CharSet *to, const CharSet *from) { vec_copy(&to->r, &from->r); }

static void set_union(CharSet *a, const CharSet *b) {
  vec_reserve(&a->r, b->r.length);
  for (size_t i = 0; i < b->r.length; ++i) {
    a->r.data[a->r.length++] = b->r.data[i];
  }
  set_normalize(a);
}

static void set_complement(CharSet *s, uint32_t max) {
  set_normalize(s);
  Vec gaps = {NULL, 0, 0};
  uint32_t next = 0;
  int done = 0;
  for (size_t i = 0; i < set_ranges(s) && !done; ++i) {
    uint32_t first = s->r.data[2 * i], last = s->r.data[2 * i + 1];
    if (first > max) {
      break;
    }
    if (first > next) {
      vec_push(&gaps, next);
      vec_push(&gaps, first - 1);
    }
    if (last >= max) {
      done = 1;
    } else {
      next = last + 1;
    }
  }
  if (!done) {
    vec_push(&gaps, next);
    vec_push(&gaps, max);
  }
  vec_free(&s->r);
  s->r = gaps;
}

static void set_intersect(CharSet *a, const CharSet *b) {
  set_normalize(a);
  Vec out = {NULL, 0, 0};
  size_t i = 0, j = 0, n = set_ranges(a), m = set_ranges(b);
  while (i < n && j < m) {
    uint32_t af = a->r.data[2 * i], al = a->r.data[2 * i + 1];
    uint32_t bf = b->r.data[2 * j], bl = b->r.data[2 * j + 1];
    uint32_t first = af > bf ? af : bf, last = al < bl ? al : bl;
    if (first <= last) {
      vec_push(&out, first);
      vec_push(&out, last);
    }
    if (al < bl) {
      ++i;
    } else {
      ++j;
    }
  }
  vec_free(&a->r);
  a->r = out;
}

static void set_subtract(CharSet *a, const CharSet *b) {
  CharSet rest = {{NULL, 0, 0}};
  set_copy(&rest, b);
  set_complement(&rest, MAX_CODE_POINT);
  set_intersect(a, &rest);
  vec_free(&rest.r);
}

static const ReOrbit *find_orbit(const ReOrbit *orbits, size_t n, uint32_t c) {
  size_t lo = 0, hi = n;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (orbits[mid].cp < c) {
      lo = mid + 1;
    } else if (orbits[mid].cp > c) {
      hi = mid;
    } else {
      return &orbits[mid];
    }
  }

  return NULL;
}

static const ReOrbit *orbits_for(int unicode, size_t *n) {
  *n = unicode ? COUNT(re_orbits_unicode) : COUNT(re_orbits_legacy);

  return unicode ? re_orbits_unicode : re_orbits_legacy;
}

uint32_t tz_re_canonicalize(uint32_t c, int unicode) {
  size_t n;
  const ReOrbit *orbits = orbits_for(unicode, &n);
  const ReOrbit *o = find_orbit(orbits, n, c);
  if (!o) {
    return c;
  }
  uint32_t least = c;
  for (uint32_t m = o->next; m != c; m = find_orbit(orbits, n, m)->next) {
    if (m < least) {
      least = m;
    }
  }

  return least;
}

static void set_close_over_case(CharSet *s, int unicode) {
  size_t n;
  const ReOrbit *orbits = orbits_for(unicode, &n);
  set_normalize(s);
  CharSet added = {{NULL, 0, 0}};
  for (size_t i = 0; i < set_ranges(s); ++i) {
    uint32_t first = s->r.data[2 * i], last = s->r.data[2 * i + 1];
    size_t lo = 0, hi = n;
    while (lo < hi) {
      size_t mid = (lo + hi) / 2;
      if (orbits[mid].cp < first) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    for (size_t k = lo; k < n && orbits[k].cp <= last; ++k) {
      for (uint32_t m = orbits[k].next; m != orbits[k].cp; m = find_orbit(orbits, n, m)->next) {
        set_add(&added, m, m);
      }
    }
  }
  set_union(s, &added);
  vec_free(&added.r);
}

static void set_fold(CharSet *s) {
  size_t n;
  const ReOrbit *orbits = orbits_for(1, &n);
  set_normalize(s);
  CharSet folded = {{NULL, 0, 0}};
  for (size_t i = 0; i < set_ranges(s); ++i) {
    uint32_t first = s->r.data[2 * i], last = s->r.data[2 * i + 1];
    size_t lo = 0, hi = n;
    while (lo < hi) {
      size_t mid = (lo + hi) / 2;
      if (orbits[mid].cp < first) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
    }
    uint32_t from = first;
    for (size_t k = lo; k < n && orbits[k].cp <= last; ++k) {
      uint32_t c = orbits[k].cp;
      if (c > from) {
        set_add(&folded, from, c - 1);
      }
      uint32_t rep = tz_re_canonicalize(c, 1);
      set_add(&folded, rep, rep);
      from = c + 1;
    }
    if (from <= last && !(from == 0 && last == 0xFFFFFFFFu)) {
      set_add(&folded, from, last);
    }
  }
  vec_free(&s->r);
  s->r = folded.r;
  set_normalize(s);
}

static void set_all_folded(CharSet *s) {
  set_add(s, 0, MAX_CODE_POINT);
  set_fold(s);
}

static void add_runs(CharSet *s, const ReRun *runs, size_t n, int (*wanted)(uint32_t, uint32_t),
                     uint32_t which) {
  for (size_t i = 0; i < n; ++i) {
    if (!wanted(runs[i].value, which)) {
      continue;
    }
    uint32_t last = i + 1 < n ? runs[i + 1].start - 1 : MAX_CODE_POINT;
    set_add(s, runs[i].start, last);
  }
  set_normalize(s);
}

static int category_wanted(uint32_t value, uint32_t mask) { return (mask >> value) & 1; }
static int script_wanted(uint32_t value, uint32_t index) { return value == index; }

static void add_list(CharSet *s, ReList list) {
  for (uint32_t i = 0; i < list.count; ++i) {
    set_add(s, re_ranges[list.offset + i].first, re_ranges[list.offset + i].last);
  }
  set_normalize(s);
}

static int in_list(ReList list, uint32_t c) {
  uint32_t lo = 0, hi = list.count;
  while (lo < hi) {
    uint32_t mid = (lo + hi) / 2;
    const ReRange *r = &re_ranges[list.offset + mid];
    if (c < r->first) {
      hi = mid;
    } else if (c > r->last) {
      lo = mid + 1;
    } else {
      return 1;
    }
  }

  return 0;
}

static const ReName *find_property_name(const char *name, int kind) {
  size_t lo = 0, hi = COUNT(re_property_names);
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    int c = strcmp(re_property_names[mid].name, name);
    if (c < 0 || (c == 0 && re_property_names[mid].kind < kind)) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  for (; lo < COUNT(re_property_names) && strcmp(re_property_names[lo].name, name) == 0; ++lo) {
    if (re_property_names[lo].kind == kind) {
      return &re_property_names[lo];
    }
  }

  return NULL;
}

static int property_set(CharSet *s, const char *name, const char *value) {
  const ReName *entry;
  if (!value) {
    entry = find_property_name(name, RE_NAME_CATEGORY);
    if (entry) {
      add_runs(s, re_categories, COUNT(re_categories), category_wanted, entry->value);
      return 1;
    }
    entry = find_property_name(name, RE_NAME_BINARY);
    if (entry) {
      add_list(s, re_binary_properties[entry->value]);
      return 1;
    }
    return 0;
  }
  if (strcmp(name, "General_Category") == 0 || strcmp(name, "gc") == 0) {
    entry = find_property_name(value, RE_NAME_CATEGORY);
    if (!entry) {
      return 0;
    }
    add_runs(s, re_categories, COUNT(re_categories), category_wanted, entry->value);
    return 1;
  }
  int extensions = strcmp(name, "Script_Extensions") == 0 || strcmp(name, "scx") == 0;
  if (!extensions && strcmp(name, "Script") != 0 && strcmp(name, "sc") != 0) {
    return 0;
  }
  entry = find_property_name(value, RE_NAME_SCRIPT);
  if (!entry) {
    return 0;
  }
  if (extensions) {
    add_list(s, re_script_extensions[entry->value]);
  } else {
    add_runs(s, re_scripts, COUNT(re_scripts), script_wanted, entry->value);
  }

  return 1;
}

static int is_id_start(uint32_t c) {
  return c == '$' || c == '_' || in_list(re_binary_properties[RE_BINARY_ID_START], c);
}

static int is_id_continue(uint32_t c) {
  return c == '$' || c == 0x200C || c == 0x200D ||
         in_list(re_binary_properties[RE_BINARY_ID_CONTINUE], c);
}

enum {
  N_EMPTY,
  N_CHAR,
  N_SET,
  N_SEQ,
  N_ALT,
  N_GROUP,
  N_REPEAT,
  N_ASSERT,
  N_LOOK,
  N_BACKREF,
};

enum { A_LINE_START, A_LINE_END, A_WORD_BOUNDARY, A_NOT_WORD_BOUNDARY };

typedef struct Node Node;
struct Node {
  int kind;
  uint32_t flags;
  uint32_t c;
  CharSet set;
  Node **kids;
  size_t count, capacity;
  Node *kid;
  uint32_t capture;
  uint32_t min, max;
  int greedy;
  int look;
  Vec refs;
  Vec name;
  size_t at;
  uint32_t first_capture, end_capture;
  Node *next_node;
};

typedef struct {
  const uint16_t *p;
  size_t length, pos;
  uint32_t flags;
  int unicode;
  int sets;
  int strict;
  const char *error;
  size_t error_at;
  int depth;
  uint32_t captures;
  uint32_t total_captures;
  int has_names;
  Vec *names;
  Vec *paths;
  Vec path;
  uint32_t disjunctions;
  Node *nodes;
  Node **backrefs;
  size_t backref_count, backref_capacity;
  Vec open;
} Parser;

static Node *new_node(Parser *ps, int kind) {
  Node *n = (Node *)re_alloc(sizeof(Node));
  memset(n, 0, sizeof(Node));
  n->kind = kind;
  n->flags = ps->flags;
  n->at = ps->pos;
  n->next_node = ps->nodes;
  ps->nodes = n;

  return n;
}

static void add_kid(Node *n, Node *kid) {
  if (n->count == n->capacity) {
    size_t capacity = n->capacity * 2 + 4;
    Node **kids = (Node **)re_alloc(capacity * sizeof(Node *));
    if (n->count) {
      memcpy(kids, n->kids, n->count * sizeof(Node *));
    }
    re_dealloc(n->kids);
    n->kids = kids;
    n->capacity = capacity;
  }
  n->kids[n->count++] = kid;
}

static void free_nodes(Parser *ps) {
  while (ps->nodes) {
    Node *n = ps->nodes;
    ps->nodes = n->next_node;
    vec_free(&n->set.r);
    vec_free(&n->refs);
    vec_free(&n->name);
    re_dealloc(n->kids);
    re_dealloc(n);
  }
}

static int fail(Parser *ps, const char *message) {
  if (!ps->error) {
    ps->error = message;
    ps->error_at = ps->pos;
  }

  return 0;
}

static Node *fail_node(Parser *ps, const char *message) {
  fail(ps, message);

  return NULL;
}

static int at_end(const Parser *ps) { return ps->pos >= ps->length; }

static uint32_t unit_at(const Parser *ps, size_t i) {
  return i < ps->length ? ps->p[i] : 0xFFFFFFFFu;
}

static uint32_t peek_unit(const Parser *ps) { return unit_at(ps, ps->pos); }

static int is_lead(uint32_t u) { return u >= 0xD800 && u <= 0xDBFF; }
static int is_trail(uint32_t u) { return u >= 0xDC00 && u <= 0xDFFF; }

static uint32_t peek_char(const Parser *ps, size_t *size) {
  uint32_t u = peek_unit(ps);
  *size = 1;
  if (ps->unicode && is_lead(u) && is_trail(unit_at(ps, ps->pos + 1))) {
    *size = 2;
    return 0x10000 + ((u - 0xD800) << 10) + (unit_at(ps, ps->pos + 1) - 0xDC00);
  }

  return u;
}

static uint32_t next_char(Parser *ps) {
  size_t size;
  uint32_t c = peek_char(ps, &size);
  ps->pos += size;

  return c;
}

static int accept_unit(Parser *ps, uint32_t u) {
  if (peek_unit(ps) != u) {
    return 0;
  }
  ++ps->pos;

  return 1;
}

static int is_digit(uint32_t c) { return c >= '0' && c <= '9'; }

static int hex_value(uint32_t c) {
  if (c >= '0' && c <= '9') {
    return (int)(c - '0');
  }
  if (c >= 'a' && c <= 'f') {
    return (int)(c - 'a' + 10);
  }
  if (c >= 'A' && c <= 'F') {
    return (int)(c - 'A' + 10);
  }

  return -1;
}

static int64_t read_hex(Parser *ps, int n) {
  int64_t value = 0;
  for (int i = 0; i < n; ++i) {
    int d = hex_value(unit_at(ps, ps->pos + (size_t)i));
    if (d < 0) {
      return -1;
    }
    value = value * 16 + d;
  }
  ps->pos += (size_t)n;

  return value;
}

static int is_syntax_char(uint32_t c) {
  return c == '^' || c == '$' || c == '\\' || c == '.' || c == '*' || c == '+' || c == '?' ||
         c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}' || c == '|';
}

static int is_line_terminator(uint32_t c) {
  return c == '\n' || c == '\r' || c == 0x2028 || c == 0x2029;
}

static int64_t read_unicode_escape(Parser *ps, int unicode) {
  size_t start = ps->pos;
  if (unicode && accept_unit(ps, '{')) {
    int64_t value = 0;
    int digits = 0;
    while (hex_value(peek_unit(ps)) >= 0) {
      value = value * 16 + hex_value(peek_unit(ps));
      ++ps->pos;
      ++digits;
      if (value > MAX_CODE_POINT) {
        ps->pos = start;
        return -1;
      }
    }
    if (digits == 0 || !accept_unit(ps, '}')) {
      ps->pos = start;
      return -1;
    }
    return value;
  }
  int64_t value = read_hex(ps, 4);
  if (value < 0) {
    return -1;
  }
  if (unicode && is_lead((uint32_t)value) && peek_unit(ps) == '\\' &&
      unit_at(ps, ps->pos + 1) == 'u') {
    size_t before = ps->pos;
    ps->pos += 2;
    int64_t low = read_hex(ps, 4);
    if (low >= 0 && is_trail((uint32_t)low)) {
      return 0x10000 + ((value - 0xD800) << 10) + (low - 0xDC00);
    }
    ps->pos = before;
  }

  return value;
}

static void scan_for_captures(Parser *ps) {
  uint32_t count = 0;
  int class_depth = 0;
  for (size_t i = 0; i < ps->length; ++i) {
    uint32_t c = ps->p[i];
    if (c == '\\') {
      ++i;
    } else if (c == '[') {
      if (class_depth == 0 || ps->sets) {
        ++class_depth;
      }
    } else if (c == ']') {
      if (class_depth > 0) {
        --class_depth;
      }
    } else if (c == '(' && class_depth == 0) {
      if (unit_at(ps, i + 1) != '?') {
        ++count;
      } else if (unit_at(ps, i + 2) == '<' && unit_at(ps, i + 3) != '=' &&
                 unit_at(ps, i + 3) != '!') {
        ++count;
        ps->has_names = 1;
      }
    }
  }
  ps->total_captures = count;
}

static int parse_group_name(Parser *ps, Vec *name) {
  int first = 1;
  for (;;) {
    if (accept_unit(ps, '>')) {
      return name->length > 0 ? 1 : fail(ps, "Invalid capture group name");
    }
    if (at_end(ps)) {
      return fail(ps, "Invalid capture group name");
    }
    uint32_t c;
    if (accept_unit(ps, '\\')) {
      if (!accept_unit(ps, 'u')) {
        return fail(ps, "Invalid Unicode escape");
      }
      int64_t value = read_unicode_escape(ps, 1);
      if (value < 0) {
        return fail(ps, "Invalid Unicode escape");
      }
      c = (uint32_t)value;
    } else {
      uint32_t u = peek_unit(ps);
      if (is_lead(u) && is_trail(unit_at(ps, ps->pos + 1))) {
        c = 0x10000 + ((u - 0xD800) << 10) + (unit_at(ps, ps->pos + 1) - 0xDC00);
        ps->pos += 2;
      } else {
        c = u;
        ++ps->pos;
      }
    }
    if (first ? !is_id_start(c) : !is_id_continue(c)) {
      return fail(ps, "Invalid capture group name");
    }
    first = 0;
    if (c >= 0x10000) {
      vec_push(name, 0xD800 + ((c - 0x10000) >> 10));
      vec_push(name, 0xDC00 + ((c - 0x10000) & 0x3FF));
    } else {
      vec_push(name, c);
    }
  }
}

static int same_name(const Vec *a, const Vec *b) {
  return a->length == b->length &&
         (a->length == 0 || memcmp(a->data, b->data, a->length * sizeof(uint32_t)) == 0);
}

static int might_both_participate(const Vec *a, const Vec *b) {
  for (size_t i = 0; i + 1 < a->length && i + 1 < b->length; i += 2) {
    if (a->data[i] != b->data[i]) {
      return 1;
    }
    if (a->data[i + 1] != b->data[i + 1]) {
      return 0;
    }
  }

  return 1;
}

static int class_escape(Parser *ps, uint32_t c, uint32_t flags, CharSet *s) {
  uint32_t lower = c | 0x20;
  if (lower != 'd' && lower != 's' && lower != 'w') {
    return 0;
  }
  if (lower == 'd') {
    set_add(s, '0', '9');
  } else if (lower == 's') {
    static const uint32_t spaces[][2] = {
        {0x09, 0x0D},     {0x20, 0x20},     {0xA0, 0xA0},     {0x1680, 0x1680}, {0x2000, 0x200A},
        {0x2028, 0x2029}, {0x202F, 0x202F}, {0x205F, 0x205F}, {0x3000, 0x3000}, {0xFEFF, 0xFEFF},
    };
    for (size_t i = 0; i < COUNT(spaces); ++i) {
      set_add(s, spaces[i][0], spaces[i][1]);
    }
  } else {
    set_add(s, '0', '9');
    set_add(s, 'A', 'Z');
    set_add(s, '_', '_');
    set_add(s, 'a', 'z');
    if (ps->unicode && (flags & TZ_RE_IGNORE_CASE)) {
      set_add(s, 0x017F, 0x017F);
      set_add(s, 0x212A, 0x212A);
    }
  }
  set_normalize(s);
  if (c != lower) {
    if (ps->sets && (flags & TZ_RE_IGNORE_CASE)) {
      CharSet all = {{NULL, 0, 0}};
      set_all_folded(&all);
      set_fold(s);
      set_subtract(&all, s);
      vec_free(&s->r);
      s->r = all.r;
    } else {
      set_complement(s, ps->unicode ? MAX_CODE_POINT : 0xFFFF);
    }
  } else if (ps->sets && (flags & TZ_RE_IGNORE_CASE)) {
    set_fold(s);
  }

  return 1;
}

static int property_escape(Parser *ps, int negated, uint32_t flags, CharSet *s) {
  if (!accept_unit(ps, '{')) {
    return fail(ps, "Invalid property name");
  }
  char name[64], value[64];
  size_t n = 0, v = 0;
  int has_value = 0;
  for (;;) {
    uint32_t c = peek_unit(ps);
    if (c == '}') {
      break;
    }
    if (c == '=' && !has_value) {
      has_value = 1;
      ++ps->pos;
      continue;
    }
    int ok =
        (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || (has_value && is_digit(c));
    if (!ok || n >= sizeof name - 1 || v >= sizeof value - 1) {
      return fail(ps, "Invalid property name");
    }
    if (has_value) {
      value[v++] = (char)c;
    } else {
      name[n++] = (char)c;
    }
    ++ps->pos;
  }
  ++ps->pos;
  name[n] = 0;
  value[v] = 0;
  static const char *const of_strings[] = {"Basic_Emoji",
                                           "Emoji_Keycap_Sequence",
                                           "RGI_Emoji_Modifier_Sequence",
                                           "RGI_Emoji_Flag_Sequence",
                                           "RGI_Emoji_Tag_Sequence",
                                           "RGI_Emoji_ZWJ_Sequence",
                                           "RGI_Emoji"};
  for (size_t i = 0; ps->sets && !has_value && !negated && i < COUNT(of_strings); ++i) {
    if (strcmp(name, of_strings[i]) == 0) {
      return fail(ps, "Properties of strings are not supported yet");
    }
  }
  if (n == 0 || (has_value && v == 0) || !property_set(s, name, has_value ? value : NULL)) {
    return fail(ps, "Invalid property name");
  }
  uint32_t max = MAX_CODE_POINT;
  if (ps->sets && (flags & TZ_RE_IGNORE_CASE)) {
    set_fold(s);
    if (negated) {
      CharSet all = {{NULL, 0, 0}};
      set_all_folded(&all);
      set_subtract(&all, s);
      vec_free(&s->r);
      s->r = all.r;
    }
  } else if (negated) {
    set_complement(s, max);
  }

  return 1;
}

static int64_t character_escape(Parser *ps, int in_class) {
  size_t start = ps->pos;
  uint32_t c = next_char(ps);
  switch (c) {
  case 'f':
    return '\f';
  case 'n':
    return '\n';
  case 'r':
    return '\r';
  case 't':
    return '\t';
  case 'v':
    return '\v';
  case 'b':
    if (in_class) {
      return '\b';
    }
    break;
  case 'c': {
    uint32_t letter = peek_unit(ps);
    if ((letter >= 'a' && letter <= 'z') || (letter >= 'A' && letter <= 'Z') ||
        (in_class && !ps->unicode && (is_digit(letter) || letter == '_'))) {
      ++ps->pos;
      return letter % 32;
    }
    if (ps->unicode) {
      fail(ps, "Invalid Unicode escape");
      return -1;
    }
    ps->pos = start;
    return '\\';
  }
  case '0':
    if (!is_digit(peek_unit(ps))) {
      return 0;
    }
    if (ps->unicode) {
      fail(ps, in_class ? "Invalid class escape" : "Invalid decimal escape");
      return -1;
    }
    break;
  case 'x': {
    int64_t value = read_hex(ps, 2);
    if (value >= 0) {
      return value;
    }
    if (ps->unicode) {
      fail(ps, "Invalid escape");
      return -1;
    }
    return 'x';
  }
  case 'u': {
    int64_t value = read_unicode_escape(ps, ps->unicode);
    if (value >= 0) {
      return value;
    }
    if (ps->unicode) {
      fail(ps, "Invalid Unicode escape");
      return -1;
    }
    return 'u';
  }
  default:
    break;
  }
  if (!ps->unicode && c >= '0' && c <= '7') {
    uint32_t value = c - '0';
    if (peek_unit(ps) >= '0' && peek_unit(ps) <= '7') {
      value = value * 8 + (peek_unit(ps) - '0');
      ++ps->pos;
      if (c <= '3' && peek_unit(ps) >= '0' && peek_unit(ps) <= '7') {
        value = value * 8 + (peek_unit(ps) - '0');
        ++ps->pos;
      }
    }
    return value;
  }
  if (ps->unicode) {
    if (is_syntax_char(c) || c == '/' || (in_class && c == '-')) {
      return c;
    }
    if (ps->sets && in_class) {
      static const char punctuators[] = "&-!#%,:;<=>@`~";
      if (c < 128 && strchr(punctuators, (int)c)) {
        return c;
      }
    }
    ps->pos = start;
    fail(ps, in_class ? "Invalid class escape" : "Invalid escape");
    return -1;
  }
  if (c == 'k' && ps->has_names) {
    ps->pos = start;
    fail(ps, in_class ? "Invalid escape" : "Invalid named reference");
    return -1;
  }

  return c;
}

static void char_set(Parser *ps, uint32_t c, uint32_t flags, CharSet *s) {
  set_add(s, c, c);
  if ((flags & TZ_RE_IGNORE_CASE) && !ps->sets) {
    set_close_over_case(s, ps->unicode);
  } else if ((flags & TZ_RE_IGNORE_CASE) && ps->sets) {
    set_fold(s);
  }
}

static int class_atom(Parser *ps, uint32_t flags, uint32_t *c, CharSet *s, int *is_set) {
  *is_set = 0;
  if (at_end(ps)) {
    return fail(ps, "Unterminated character class");
  }
  if (!accept_unit(ps, '\\')) {
    *c = next_char(ps);
    return 1;
  }
  if (at_end(ps)) {
    return fail(ps, "\\ at end of pattern");
  }
  uint32_t e = peek_unit(ps);
  if (class_escape(ps, e, flags, s)) {
    ++ps->pos;
    *is_set = 1;
    return 1;
  }
  if ((e == 'p' || e == 'P') && ps->unicode) {
    ++ps->pos;
    *is_set = 1;
    return property_escape(ps, e == 'P', flags, s);
  }
  if (e == 'k' && !ps->unicode && !ps->has_names) {
    ++ps->pos;
    *c = 'k';
    return 1;
  }
  int64_t value = character_escape(ps, 1);
  if (value < 0) {
    return 0;
  }
  *c = (uint32_t)value;

  return 1;
}

static Node *parse_class(Parser *ps) {
  Node *n = new_node(ps, N_SET);
  n->at = ps->pos - 1;
  uint32_t flags = ps->flags;
  int negated = accept_unit(ps, '^');
  CharSet *s = &n->set;
  int after_lead = 0;
  for (;;) {
    if (at_end(ps)) {
      return fail_node(ps, "Unterminated character class");
    }
    if (accept_unit(ps, ']')) {
      break;
    }
    uint32_t a = 0, b = 0;
    int a_set, b_set;
    CharSet atom = {{NULL, 0, 0}};
    if (!class_atom(ps, flags, &a, &atom, &a_set)) {
      vec_free(&atom.r);
      return NULL;
    }
    if (ps->strict && !ps->unicode && !a_set && after_lead && is_trail(a)) {
      vec_free(&atom.r);
      return fail_node(ps, "without the u flag, a class takes a character written with two "
                           "code units (such as an emoji) as two separate halves; add the u flag");
    }
    after_lead = !a_set && is_lead(a);
    if (peek_unit(ps) == '-' && unit_at(ps, ps->pos + 1) != ']' && ps->pos + 1 < ps->length) {
      ++ps->pos;
      CharSet other = {{NULL, 0, 0}};
      if (!class_atom(ps, flags, &b, &other, &b_set)) {
        vec_free(&atom.r);
        vec_free(&other.r);
        return NULL;
      }
      if (a_set || b_set) {
        if (ps->unicode) {
          vec_free(&atom.r);
          vec_free(&other.r);
          return fail_node(ps, "Invalid character class");
        }
        if (!a_set) {
          set_add(&atom, a, a);
        }
        if (!b_set) {
          set_add(&other, b, b);
        }
        set_add(&atom, '-', '-');
        set_union(&atom, &other);
        vec_free(&other.r);
        set_union(s, &atom);
        vec_free(&atom.r);
        continue;
      }
      vec_free(&other.r);
      if (a > b) {
        vec_free(&atom.r);
        return fail_node(ps, "Range out of order in character class");
      }
      set_add(s, a, b);
    } else if (a_set) {
      set_union(s, &atom);
    } else {
      set_add(s, a, a);
    }
    vec_free(&atom.r);
  }
  set_normalize(s);
  if (flags & TZ_RE_IGNORE_CASE) {
    set_close_over_case(s, ps->unicode);
  }
  if (negated) {
    set_complement(s, ps->unicode ? MAX_CODE_POINT : 0xFFFF);
  }

  return n;
}

typedef struct {
  CharSet chars;
  Vec *strings;
  size_t count, capacity;
} ClassSet;

static void class_free(ClassSet *c) {
  vec_free(&c->chars.r);
  for (size_t i = 0; i < c->count; ++i) {
    vec_free(&c->strings[i]);
  }
  re_dealloc(c->strings);
  c->strings = NULL;
  c->count = c->capacity = 0;
}

static int class_has_string(const ClassSet *c, const Vec *s) {
  for (size_t i = 0; i < c->count; ++i) {
    if (same_name(&c->strings[i], s)) {
      return 1;
    }
  }

  return 0;
}

static void class_add_string(ClassSet *c, const Vec *s) {
  if (class_has_string(c, s)) {
    return;
  }
  if (c->count == c->capacity) {
    size_t capacity = c->capacity * 2 + 4;
    Vec *strings = (Vec *)re_alloc(capacity * sizeof(Vec));
    if (c->count) {
      memcpy(strings, c->strings, c->count * sizeof(Vec));
    }
    re_dealloc(c->strings);
    c->strings = strings;
    c->capacity = capacity;
  }
  Vec copy = {NULL, 0, 0};
  vec_copy(&copy, s);
  c->strings[c->count++] = copy;
}

static void class_union(ClassSet *a, ClassSet *b) {
  set_union(&a->chars, &b->chars);
  for (size_t i = 0; i < b->count; ++i) {
    class_add_string(a, &b->strings[i]);
  }
}

static void class_intersect(ClassSet *a, ClassSet *b) {
  set_intersect(&a->chars, &b->chars);
  size_t kept = 0;
  for (size_t i = 0; i < a->count; ++i) {
    if (class_has_string(b, &a->strings[i])) {
      a->strings[kept++] = a->strings[i];
    } else {
      vec_free(&a->strings[i]);
    }
  }
  a->count = kept;
}

static void class_subtract(ClassSet *a, ClassSet *b) {
  set_subtract(&a->chars, &b->chars);
  size_t kept = 0;
  for (size_t i = 0; i < a->count; ++i) {
    if (!class_has_string(b, &a->strings[i])) {
      a->strings[kept++] = a->strings[i];
    } else {
      vec_free(&a->strings[i]);
    }
  }
  a->count = kept;
}

static int parse_class_contents(Parser *ps, ClassSet *result);

static int parse_class_strings(Parser *ps, ClassSet *c) {
  if (!accept_unit(ps, '{')) {
    return fail(ps, "Invalid escape");
  }
  Vec s = {NULL, 0, 0};
  for (;;) {
    if (at_end(ps)) {
      vec_free(&s);
      return fail(ps, "Unterminated character class");
    }
    uint32_t u = peek_unit(ps);
    if (u == '|' || u == '}') {
      ++ps->pos;
      if (s.length == 1) {
        set_add(&c->chars, s.data[0], s.data[0]);
      } else {
        class_add_string(c, &s);
      }
      s.length = 0;
      if (u == '}') {
        break;
      }
      continue;
    }
    uint32_t ch;
    if (accept_unit(ps, '\\')) {
      int64_t value = character_escape(ps, 1);
      if (value < 0) {
        vec_free(&s);
        return 0;
      }
      ch = (uint32_t)value;
    } else {
      ch = next_char(ps);
    }
    if (ps->flags & TZ_RE_IGNORE_CASE) {
      ch = tz_re_canonicalize(ch, 1);
    }
    vec_push(&s, ch);
  }
  vec_free(&s);

  return 1;
}

static int is_reserved_double(const Parser *ps) {
  static const char doubles[] = "&!#$%*+,.:;<=>?@^`~";
  uint32_t c = peek_unit(ps);

  return c < 128 && c != 0 && strchr(doubles, (int)c) && unit_at(ps, ps->pos + 1) == c;
}

static int is_set_syntax(uint32_t c) {
  return c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}' || c == '/' ||
         c == '-' || c == '\\' || c == '|';
}

static int parse_class_operand(Parser *ps, ClassSet *c, uint32_t *single, int *is_char) {
  *is_char = 0;
  uint32_t flags = ps->flags;
  if (accept_unit(ps, '[')) {
    if (++ps->depth > MAX_DEPTH) {
      return fail(ps, "Regular expression too large");
    }
    int ok = parse_class_contents(ps, c);
    --ps->depth;
    return ok;
  }
  if (accept_unit(ps, '\\')) {
    if (at_end(ps)) {
      return fail(ps, "\\ at end of pattern");
    }
    uint32_t e = peek_unit(ps);
    if (e == 'q') {
      ++ps->pos;
      return parse_class_strings(ps, c);
    }
    if (class_escape(ps, e, flags, &c->chars)) {
      ++ps->pos;
      return 1;
    }
    if (e == 'p' || e == 'P') {
      ++ps->pos;
      return property_escape(ps, e == 'P', flags, &c->chars);
    }
    int64_t value = character_escape(ps, 1);
    if (value < 0) {
      return 0;
    }
    *single = (uint32_t)value;
    *is_char = 1;
    return 1;
  }
  if (at_end(ps)) {
    return fail(ps, "Unterminated character class");
  }
  if (is_reserved_double(ps)) {
    return fail(ps, "Invalid set operation in character class");
  }
  uint32_t ch = peek_unit(ps);
  if (is_set_syntax(ch)) {
    return fail(ps, "Invalid character in character class");
  }
  *single = next_char(ps);
  *is_char = 1;

  return 1;
}

static int at_operator(const Parser *ps, uint32_t c) {
  return peek_unit(ps) == c && unit_at(ps, ps->pos + 1) == c;
}

static int parse_class_element(Parser *ps, ClassSet *c, int range_ok) {
  uint32_t a, b;
  int is_char, result = 1;
  if (!parse_class_operand(ps, c, &a, &is_char)) {
    return 0;
  }
  if (!is_char) {
    return 1;
  }
  b = a;
  if (peek_unit(ps) == '-' && unit_at(ps, ps->pos + 1) != '-') {
    result = 2;
    if (!range_ok) {
      return fail(ps, "Invalid set operation in character class");
    }
    ++ps->pos;
    int b_char;
    ClassSet other = {{{NULL, 0, 0}}, NULL, 0, 0};
    int ok = parse_class_operand(ps, &other, &b, &b_char);
    class_free(&other);
    if (!ok) {
      return 0;
    }
    if (!b_char) {
      return fail(ps, "Invalid character class");
    }
    if (a > b) {
      return fail(ps, "Range out of order in character class");
    }
  }
  set_add(&c->chars, a, b);
  set_normalize(&c->chars);
  if (ps->flags & TZ_RE_IGNORE_CASE) {
    set_fold(&c->chars);
  }

  return result;
}

static int parse_class_contents(Parser *ps, ClassSet *result) {
  int negated = accept_unit(ps, '^');
  int fold = (ps->flags & TZ_RE_IGNORE_CASE) != 0;
  ClassSet acc = {{{NULL, 0, 0}}, NULL, 0, 0};
  int ok = 1;
  if (!accept_unit(ps, ']')) {
    int first = parse_class_element(ps, &acc, 1);
    ok = first != 0;
    uint32_t op = at_operator(ps, '&') ? '&' : at_operator(ps, '-') ? '-' : 0;
    if (ok && op) {
      if (first == 2) {
        ok = fail(ps, "Invalid set operation in character class");
      }
      while (ok && at_operator(ps, op)) {
        ps->pos += 2;
        if (op == '&' && peek_unit(ps) == '&') {
          ok = fail(ps, "Invalid character in character class");
          break;
        }
        ClassSet operand = {{{NULL, 0, 0}}, NULL, 0, 0};
        ok = parse_class_element(ps, &operand, 0);
        if (ok) {
          if (op == '&') {
            class_intersect(&acc, &operand);
          } else {
            class_subtract(&acc, &operand);
          }
        }
        class_free(&operand);
      }
      if (ok && !accept_unit(ps, ']')) {
        ok = at_end(ps) ? fail(ps, "Unterminated character class")
                        : fail(ps, "Invalid set operation in character class");
      }
    } else {
      while (ok && !accept_unit(ps, ']')) {
        if (at_end(ps)) {
          ok = fail(ps, "Unterminated character class");
          break;
        }
        if (at_operator(ps, '&') || at_operator(ps, '-')) {
          ok = fail(ps, "Invalid set operation in character class");
          break;
        }
        ClassSet operand = {{{NULL, 0, 0}}, NULL, 0, 0};
        ok = parse_class_element(ps, &operand, 1);
        if (ok) {
          class_union(&acc, &operand);
        }
        class_free(&operand);
      }
    }
  }
  if (!ok) {
    class_free(&acc);
    return 0;
  }
  if (negated) {
    if (acc.count > 0) {
      class_free(&acc);
      return fail(ps, "Negated character class may contain strings");
    }
    if (fold) {
      CharSet all = {{NULL, 0, 0}};
      set_all_folded(&all);
      set_subtract(&all, &acc.chars);
      vec_free(&acc.chars.r);
      acc.chars = all;
    } else {
      set_complement(&acc.chars, MAX_CODE_POINT);
    }
  }
  class_union(result, &acc);
  class_free(&acc);

  return 1;
}

static int compare_strings_longest(const void *a, const void *b) {
  size_t x = ((const Vec *)a)->length, y = ((const Vec *)b)->length;

  return x > y ? -1 : x < y;
}

static Node *parse_class_set(Parser *ps) {
  size_t at = ps->pos - 1;
  ClassSet c = {{{NULL, 0, 0}}, NULL, 0, 0};
  if (!parse_class_contents(ps, &c)) {
    class_free(&c);
    return NULL;
  }
  int fold = (ps->flags & TZ_RE_IGNORE_CASE) != 0;
  if (fold) {
    set_close_over_case(&c.chars, 1);
  }
  Node *set = new_node(ps, N_SET);
  set->at = at;
  set->set = c.chars;
  c.chars.r.data = NULL;
  c.chars.r.length = c.chars.r.capacity = 0;
  if (c.count == 0) {
    class_free(&c);
    return set;
  }
  qsort(c.strings, c.count, sizeof(Vec), compare_strings_longest);
  Node *alt = new_node(ps, N_ALT);
  for (size_t i = 0; i < c.count; ++i) {
    Node *seq = new_node(ps, N_SEQ);
    for (size_t k = 0; k < c.strings[i].length; ++k) {
      Node *ch = new_node(ps, N_SET);
      char_set(ps, c.strings[i].data[k], 0, &ch->set);
      if (fold) {
        set_close_over_case(&ch->set, 1);
      }
      add_kid(seq, ch);
    }
    add_kid(alt, seq);
  }
  if (set_ranges(&set->set) > 0) {
    add_kid(alt, set);
  }
  class_free(&c);

  return alt;
}

static Node *parse_disjunction(Parser *ps);

static Node *char_node(Parser *ps, uint32_t c) {
  if (!(ps->flags & TZ_RE_IGNORE_CASE)) {
    Node *n = new_node(ps, N_CHAR);
    n->c = c;
    return n;
  }
  Node *n = new_node(ps, N_SET);
  set_add(&n->set, c, c);
  set_close_over_case(&n->set, ps->unicode);
  if (set_ranges(&n->set) == 1 && n->set.r.data[0] == n->set.r.data[1]) {
    vec_free(&n->set.r);
    n->kind = N_CHAR;
    n->c = c;
  }

  return n;
}

static int read_braces(Parser *ps, uint32_t *min, uint32_t *max) {
  size_t start = ps->pos;
  if (!accept_unit(ps, '{')) {
    return 0;
  }
  uint64_t a = 0, b = 0;
  int digits = 0;
  while (is_digit(peek_unit(ps))) {
    a = a * 10 + (peek_unit(ps) - '0');
    if (a > RE_INFINITY - 1) {
      a = RE_INFINITY - 1;
    }
    ++ps->pos;
    ++digits;
  }
  if (!digits) {
    ps->pos = start;
    return 0;
  }
  b = a;
  if (accept_unit(ps, ',')) {
    b = RE_INFINITY;
    if (is_digit(peek_unit(ps))) {
      b = 0;
      while (is_digit(peek_unit(ps))) {
        b = b * 10 + (peek_unit(ps) - '0');
        if (b > RE_INFINITY - 1) {
          b = RE_INFINITY - 1;
        }
        ++ps->pos;
      }
    }
  }
  if (!accept_unit(ps, '}')) {
    ps->pos = start;
    return 0;
  }
  *min = (uint32_t)a;
  *max = (uint32_t)b;

  return 1;
}

static Node *parse_quantifier(Parser *ps, Node *atom) {
  uint32_t min, max;
  size_t at = ps->pos;
  uint32_t c = peek_unit(ps);
  if (c == '*') {
    min = 0;
    max = RE_INFINITY;
    ++ps->pos;
  } else if (c == '+') {
    min = 1;
    max = RE_INFINITY;
    ++ps->pos;
  } else if (c == '?') {
    min = 0;
    max = 1;
    ++ps->pos;
  } else if (c == '{') {
    if (!read_braces(ps, &min, &max)) {
      if (ps->unicode) {
        return fail_node(ps, "Incomplete quantifier");
      }
      return atom;
    }
    if (min > max) {
      ps->pos = at;
      return fail_node(ps, "numbers out of order in {} quantifier");
    }
  } else {
    return atom;
  }
  Node *n = new_node(ps, N_REPEAT);
  n->at = at;
  n->kid = atom;
  n->min = min;
  n->max = max;
  n->greedy = !accept_unit(ps, '?');
  n->first_capture = atom->first_capture;
  n->end_capture = atom->end_capture;

  return n;
}

static int is_quantifier_start(Parser *ps) {
  uint32_t c = peek_unit(ps);
  if (c == '*' || c == '+' || c == '?') {
    return 1;
  }
  if (c != '{') {
    return 0;
  }
  uint32_t min, max;
  size_t start = ps->pos;
  int braces = read_braces(ps, &min, &max);
  ps->pos = start;

  return braces;
}

static int parse_modifiers(Parser *ps, uint32_t *flags) {
  uint32_t add = 0, remove = 0;
  int dash = 0;
  for (;;) {
    uint32_t c = peek_unit(ps);
    uint32_t bit = c == 'i'   ? TZ_RE_IGNORE_CASE
                   : c == 'm' ? TZ_RE_MULTILINE
                   : c == 's' ? TZ_RE_DOT_ALL
                              : 0;
    if (c == ':') {
      ++ps->pos;
      break;
    }
    if (c == '-') {
      if (dash) {
        return fail(ps, "Multiple dashes in flag group");
      }
      dash = 1;
      ++ps->pos;
      continue;
    }
    if (!bit) {
      return fail(ps, "Invalid group");
    }
    if ((add | remove) & bit) {
      return fail(ps, "Repeated flag in flag group");
    }
    if (dash) {
      remove |= bit;
    } else {
      add |= bit;
    }
    ++ps->pos;
  }
  if (dash && !remove && !add) {
    return fail(ps, "Invalid flag group");
  }
  *flags = (ps->flags | add) & ~remove;

  return 1;
}

static Node *parse_group(Parser *ps, int *quantifiable) {
  size_t at = ps->pos - 1;
  *quantifiable = 1;
  if (!accept_unit(ps, '?')) {
    uint32_t index = ++ps->captures;
    if (index > MAX_CAPTURES) {
      return fail_node(ps, "Too many captures");
    }
    vec_copy(&ps->paths[index], &ps->path);
    Node *n = new_node(ps, N_GROUP);
    n->at = at;
    n->capture = index;
    vec_push(&ps->open, index);
    Node *kid = parse_disjunction(ps);
    if (!kid) {
      return NULL;
    }
    --ps->open.length;
    if (!accept_unit(ps, ')')) {
      return fail_node(ps, "Unterminated group");
    }
    n->kid = kid;
    n->first_capture = index;
    n->end_capture = ps->captures + 1;
    return n;
  }
  uint32_t c = peek_unit(ps);
  int look = -1;
  if (c == '=' || c == '!') {
    ++ps->pos;
    look = c == '!' ? RE_LOOK_NEGATIVE : 0;
  } else if (c == '<' && (unit_at(ps, ps->pos + 1) == '=' || unit_at(ps, ps->pos + 1) == '!')) {
    look = RE_LOOK_BEHIND | (unit_at(ps, ps->pos + 1) == '!' ? RE_LOOK_NEGATIVE : 0);
    ps->pos += 2;
  }
  if (look >= 0) {
    Node *n = new_node(ps, N_LOOK);
    n->at = at;
    n->look = look;
    uint32_t first = ps->captures + 1;
    Node *kid = parse_disjunction(ps);
    if (!kid) {
      return NULL;
    }
    if (!accept_unit(ps, ')')) {
      return fail_node(ps, "Unterminated group");
    }
    n->kid = kid;
    n->first_capture = first;
    n->end_capture = ps->captures + 1;
    *quantifiable = !ps->unicode && !(look & RE_LOOK_BEHIND);
    return n;
  }
  if (c == '<') {
    ++ps->pos;
    uint32_t index = ++ps->captures;
    if (index > MAX_CAPTURES) {
      return fail_node(ps, "Too many captures");
    }
    Vec *name = &ps->names[index];
    if (!parse_group_name(ps, name)) {
      return NULL;
    }
    vec_copy(&ps->paths[index], &ps->path);
    for (uint32_t k = 1; k < index; ++k) {
      if (same_name(&ps->names[k], name) &&
          might_both_participate(&ps->paths[k], &ps->paths[index])) {
        ps->pos = at;
        return fail_node(ps, "Duplicate capture group name");
      }
    }
    Node *n = new_node(ps, N_GROUP);
    n->at = at;
    n->capture = index;
    vec_push(&ps->open, index);
    Node *kid = parse_disjunction(ps);
    if (!kid) {
      return NULL;
    }
    --ps->open.length;
    if (!accept_unit(ps, ')')) {
      return fail_node(ps, "Unterminated group");
    }
    n->kid = kid;
    n->first_capture = index;
    n->end_capture = ps->captures + 1;
    return n;
  }
  uint32_t flags = ps->flags;
  if (c != ':' && !parse_modifiers(ps, &flags)) {
    return ps->error ? NULL : fail_node(ps, "Invalid group");
  }
  if (c == ':') {
    ++ps->pos;
  }
  uint32_t outer = ps->flags;
  ps->flags = flags;
  uint32_t first = ps->captures + 1;
  Node *kid = parse_disjunction(ps);
  ps->flags = outer;
  if (!kid) {
    return NULL;
  }
  if (!accept_unit(ps, ')')) {
    return fail_node(ps, "Unterminated group");
  }
  Node *n = new_node(ps, N_GROUP);
  n->at = at;
  n->kid = kid;
  n->first_capture = first;
  n->end_capture = ps->captures + 1;

  return n;
}

static Node *parse_atom_escape(Parser *ps) {
  size_t at = ps->pos - 1;
  if (at_end(ps)) {
    return fail_node(ps, "\\ at end of pattern");
  }
  uint32_t c = peek_unit(ps);
  if (c == 'b' || c == 'B') {
    ++ps->pos;
    Node *n = new_node(ps, N_ASSERT);
    n->c = c == 'b' ? A_WORD_BOUNDARY : A_NOT_WORD_BOUNDARY;
    return n;
  }
  if (c >= '1' && c <= '9') {
    size_t start = ps->pos;
    uint64_t number = 0;
    while (is_digit(peek_unit(ps)) && number <= MAX_CAPTURES) {
      number = number * 10 + (peek_unit(ps) - '0');
      ++ps->pos;
    }
    if (number <= ps->total_captures) {
      for (size_t i = 0; i < ps->open.length; ++i) {
        if (ps->open.data[i] == number) {
          return new_node(ps, N_EMPTY);
        }
      }
      Node *n = new_node(ps, N_BACKREF);
      n->at = at;
      vec_push(&n->refs, (uint32_t)number);
      return n;
    }
    ps->pos = start;
    if (ps->unicode) {
      return fail_node(ps, "Invalid escape");
    }
    if (ps->strict) {
      ps->pos = at;
      return fail_node(ps, "there is no such group to refer to (JavaScript would read an octal "
                           "escape here); write a character as '\\xNN'");
    }
  }
  if (c == 'k' && (ps->unicode || ps->has_names)) {
    ++ps->pos;
    if (!accept_unit(ps, '<')) {
      return fail_node(ps, "Invalid named reference");
    }
    Node *n = new_node(ps, N_BACKREF);
    n->at = at;
    if (!parse_group_name(ps, &n->name)) {
      ps->error = "Invalid named reference";
      return NULL;
    }
    for (size_t i = 0; i < ps->open.length; ++i) {
      if (same_name(&ps->names[ps->open.data[i]], &n->name)) {
        n->kind = N_EMPTY;
        return n;
      }
    }
    if (ps->backref_count == ps->backref_capacity) {
      size_t capacity = ps->backref_capacity * 2 + 4;
      Node **refs = (Node **)re_alloc(capacity * sizeof(Node *));
      if (ps->backref_count) {
        memcpy(refs, ps->backrefs, ps->backref_count * sizeof(Node *));
      }
      re_dealloc(ps->backrefs);
      ps->backrefs = refs;
      ps->backref_capacity = capacity;
    }
    ps->backrefs[ps->backref_count++] = n;
    return n;
  }
  Node *set = new_node(ps, N_SET);
  set->at = at;
  if (class_escape(ps, c, ps->flags, &set->set)) {
    ++ps->pos;
    if (ps->flags & TZ_RE_IGNORE_CASE) {
      if (ps->sets) {
        set_close_over_case(&set->set, 1);
      } else {
        set_close_over_case(&set->set, ps->unicode);
      }
    }
    return set;
  }
  if ((c == 'p' || c == 'P') && !ps->unicode && ps->strict) {
    return fail_node(ps, "'\\p' needs the u or v flag: without it, '\\p{L}' is the text 'p{L}'");
  }
  if ((c == 'p' || c == 'P') && ps->unicode) {
    ++ps->pos;
    if (!property_escape(ps, c == 'P', ps->flags, &set->set)) {
      return NULL;
    }
    if (ps->flags & TZ_RE_IGNORE_CASE) {
      set_close_over_case(&set->set, 1);
    }
    return set;
  }
  int64_t value = character_escape(ps, 0);
  if (value < 0) {
    return NULL;
  }

  return char_node(ps, (uint32_t)value);
}

static Node *parse_term(Parser *ps) {
  if (at_end(ps)) {
    return NULL;
  }
  size_t at = ps->pos;
  uint32_t c = peek_unit(ps);
  Node *atom = NULL;
  int quantifiable = 1;
  switch (c) {
  case '|':
  case ')':
    return NULL;
  case '^':
  case '$':
    ++ps->pos;
    atom = new_node(ps, N_ASSERT);
    atom->c = c == '^' ? A_LINE_START : A_LINE_END;
    quantifiable = 0;
    break;
  case '(':
    ++ps->pos;
    if (++ps->depth > MAX_DEPTH) {
      return fail_node(ps, "Regular expression too large");
    }
    atom = parse_group(ps, &quantifiable);
    --ps->depth;
    if (!atom) {
      return NULL;
    }
    break;
  case '.': {
    ++ps->pos;
    atom = new_node(ps, N_SET);
    if (ps->flags & TZ_RE_DOT_ALL) {
      set_add(&atom->set, 0, ps->unicode ? MAX_CODE_POINT : 0xFFFF);
    } else {
      set_add(&atom->set, '\n', '\n');
      set_add(&atom->set, '\r', '\r');
      set_add(&atom->set, 0x2028, 0x2029);
      set_complement(&atom->set, ps->unicode ? MAX_CODE_POINT : 0xFFFF);
    }
    break;
  }
  case '[':
    ++ps->pos;
    atom = ps->sets ? parse_class_set(ps) : parse_class(ps);
    if (!atom) {
      return NULL;
    }
    break;
  case '\\':
    ++ps->pos;
    atom = parse_atom_escape(ps);
    if (!atom) {
      return NULL;
    }
    if (atom->kind == N_ASSERT) {
      quantifiable = 0;
    }
    break;
  case '*':
  case '+':
  case '?':
    return fail_node(ps, "Nothing to repeat");
  case '{':
    if (is_quantifier_start(ps)) {
      return fail_node(ps, "Nothing to repeat");
    }
    if (ps->unicode) {
      return fail_node(ps, "Lone quantifier brackets");
    }
    ++ps->pos;
    atom = char_node(ps, '{');
    break;
  case '}':
  case ']':
    if (ps->unicode) {
      return fail_node(ps, "Lone quantifier brackets");
    }
    ++ps->pos;
    atom = char_node(ps, c);
    break;
  default:
    atom = char_node(ps, next_char(ps));
    break;
  }
  atom->at = at;
  if (!quantifiable) {
    if (is_quantifier_start(ps)) {
      return fail_node(ps, atom->kind == N_LOOK ? "Invalid quantifier" : "Nothing to repeat");
    }
    return atom;
  }

  return parse_quantifier(ps, atom);
}

static Node *parse_alternative(Parser *ps) {
  Node *seq = new_node(ps, N_SEQ);
  seq->first_capture = ps->captures + 1;
  for (;;) {
    Node *term = parse_term(ps);
    if (ps->error) {
      return NULL;
    }
    if (!term) {
      break;
    }
    add_kid(seq, term);
  }
  seq->end_capture = ps->captures + 1;

  return seq;
}

static Node *parse_disjunction(Parser *ps) {
  uint32_t id = ps->disjunctions++;
  uint32_t first = ps->captures + 1;
  vec_push(&ps->path, id);
  vec_push(&ps->path, 0);
  Node *first_alt = parse_alternative(ps);
  if (!first_alt) {
    return NULL;
  }
  if (peek_unit(ps) != '|') {
    ps->path.length -= 2;
    return first_alt;
  }
  Node *alt = new_node(ps, N_ALT);
  add_kid(alt, first_alt);
  for (uint32_t index = 1; accept_unit(ps, '|'); ++index) {
    ps->path.data[ps->path.length - 1] = index;
    Node *next = parse_alternative(ps);
    if (!next) {
      return NULL;
    }
    add_kid(alt, next);
  }
  ps->path.length -= 2;
  alt->first_capture = first;
  alt->end_capture = ps->captures + 1;

  return alt;
}

static int nullable(const Node *n) {
  switch (n->kind) {
  case N_CHAR:
  case N_SET:
    return 0;
  case N_SEQ:
    for (size_t i = 0; i < n->count; ++i) {
      if (!nullable(n->kids[i])) {
        return 0;
      }
    }
    return 1;
  case N_ALT:
    for (size_t i = 0; i < n->count; ++i) {
      if (nullable(n->kids[i])) {
        return 1;
      }
    }
    return 0;
  case N_GROUP:
    return nullable(n->kid);
  case N_REPEAT:
    return n->min == 0 || nullable(n->kid);
  default:
    return 1;
  }
}

static void mandatory(const Parser *ps, const Node *n, uint32_t *set, int by_name) {
  size_t words = (ps->captures + 1 + 31) / 32;
  switch (n->kind) {
  case N_SEQ:
    for (size_t i = 0; i < n->count; ++i) {
      mandatory(ps, n->kids[i], set, by_name);
    }
    return;
  case N_ALT: {
    uint32_t *all = (uint32_t *)re_alloc(words * sizeof(uint32_t));
    uint32_t *one = (uint32_t *)re_alloc(words * sizeof(uint32_t));
    for (size_t w = 0; w < words; ++w) {
      all[w] = 0xFFFFFFFFu;
    }
    for (size_t i = 0; i < n->count; ++i) {
      memset(one, 0, words * sizeof(uint32_t));
      mandatory(ps, n->kids[i], one, by_name);
      for (size_t w = 0; w < words; ++w) {
        all[w] &= one[w];
      }
    }
    for (size_t w = 0; w < words; ++w) {
      set[w] |= all[w];
    }
    re_dealloc(all);
    re_dealloc(one);
    return;
  }
  case N_GROUP:
    if (n->capture) {
      set[n->capture / 32] |= 1u << (n->capture % 32);
      if (by_name && ps->names[n->capture].length) {
        for (uint32_t k = 1; k <= ps->captures; ++k) {
          if (same_name(&ps->names[k], &ps->names[n->capture])) {
            set[k / 32] |= 1u << (k % 32);
          }
        }
      }
    }
    mandatory(ps, n->kid, set, by_name);
    return;
  case N_REPEAT:
    if (n->min > 0) {
      mandatory(ps, n->kid, set, by_name);
    }
    return;
  case N_LOOK:
    if (!(n->look & RE_LOOK_NEGATIVE)) {
      mandatory(ps, n->kid, set, by_name);
    }
    return;
  default:
    return;
  }
}

static int first_units(const Node *n, uint32_t *bits, int *wide, int unicode) {
  switch (n->kind) {
  case N_CHAR: {
    uint32_t c = n->c;
    if (c >= 0x10000) {
      c = 0xD800 + ((c - 0x10000) >> 10);
    }
    if (c < 256) {
      bits[c / 32] |= 1u << (c % 32);
    } else {
      *wide = 1;
    }
    return 1;
  }
  case N_SET:
    for (size_t i = 0; i < set_ranges(&n->set); ++i) {
      uint32_t first = n->set.r.data[2 * i], last = n->set.r.data[2 * i + 1];
      for (uint32_t c = first; c <= last && c < 256; ++c) {
        bits[c / 32] |= 1u << (c % 32);
      }
      if (last >= 256) {
        *wide = 1;
      }
    }
    (void)unicode;
    return 1;
  case N_SEQ:
    for (size_t i = 0; i < n->count; ++i) {
      const Node *kid = n->kids[i];
      if (kid->kind == N_ASSERT || kid->kind == N_LOOK) {
        continue;
      }
      if (!first_units(kid, bits, wide, unicode)) {
        return 0;
      }
      if (!nullable(kid)) {
        return 1;
      }
    }
    return 0;
  case N_ALT:
    for (size_t i = 0; i < n->count; ++i) {
      if (!first_units(n->kids[i], bits, wide, unicode)) {
        return 0;
      }
    }
    return 1;
  case N_GROUP:
    return first_units(n->kid, bits, wide, unicode);
  case N_REPEAT:
    if (n->max == 0) {
      return 0;
    }
    return first_units(n->kid, bits, wide, unicode);
  default:
    return 0;
  }
}

static int anchored(const Node *n) {
  switch (n->kind) {
  case N_ASSERT:
    return n->c == A_LINE_START && !(n->flags & TZ_RE_MULTILINE);
  case N_SEQ:
    return n->count > 0 && anchored(n->kids[0]);
  case N_ALT:
    for (size_t i = 0; i < n->count; ++i) {
      if (!anchored(n->kids[i])) {
        return 0;
      }
    }
    return n->count > 0;
  case N_GROUP:
    return anchored(n->kid);
  default:
    return 0;
  }
}

typedef struct {
  Vec code;
  uint32_t registers;
  uint32_t captures;
  int unicode;
} Emitter;

static void emit(Emitter *e, uint32_t word) { vec_push(&e->code, word); }

static uint32_t here(const Emitter *e) { return (uint32_t)e->code.length; }

static void emit_set(Emitter *e, const CharSet *s, int back) {
  uint32_t max = e->unicode ? MAX_CODE_POINT : 0xFFFF;
  size_t n = set_ranges(s);
  if (n == 1 && s->r.data[0] == 0 && s->r.data[1] >= max) {
    emit(e, back ? RE_ALL_BACK : RE_ALL);
    return;
  }
  if (n == 4 && s->r.data[0] == 0 && s->r.data[1] == 9 && s->r.data[2] == 11 &&
      s->r.data[3] == 12 && s->r.data[4] == 14 && s->r.data[5] == 0x2027 &&
      s->r.data[6] == 0x202A && s->r.data[7] >= max) {
    emit(e, back ? RE_ANY_BACK : RE_ANY);
    return;
  }
  if (n == 1 && s->r.data[0] == s->r.data[1]) {
    emit(e, back ? RE_CHAR_BACK : RE_CHAR);
    emit(e, s->r.data[0]);
    return;
  }
  emit(e, back ? RE_SET_BACK : RE_SET);
  size_t count_at = e->code.length;
  emit(e, 0);
  uint32_t bits[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  for (size_t i = 0; i < n; ++i) {
    for (uint32_t c = s->r.data[2 * i]; c <= s->r.data[2 * i + 1] && c < 256; ++c) {
      bits[c / 32] |= 1u << (c % 32);
    }
  }
  for (int i = 0; i < 8; ++i) {
    emit(e, bits[i]);
  }
  uint32_t wide = 0;
  for (size_t i = 0; i < n; ++i) {
    uint32_t first = s->r.data[2 * i], last = s->r.data[2 * i + 1];
    if (last < 256) {
      continue;
    }
    emit(e, first < 256 ? 256 : first);
    emit(e, last);
    ++wide;
  }
  e->code.data[count_at] = wide;
}

static int is_single(const Node *n) { return n->kind == N_CHAR || n->kind == N_SET; }

static void emit_node(Emitter *e, const Node *n, int back);

static void emit_group(Emitter *e, const Node *n, int back) {
  if (!n->capture) {
    emit_node(e, n->kid, back);
    return;
  }
  uint32_t r = e->registers++;
  emit(e, RE_SET_POSITION);
  emit(e, r);
  emit_node(e, n->kid, back);
  emit(e, back ? RE_SAVE_BACK : RE_SAVE);
  emit(e, n->capture);
  emit(e, r);
}

static void emit_reset(Emitter *e, const Node *n) {
  if (n->first_capture < n->end_capture) {
    emit(e, RE_RESET);
    emit(e, n->first_capture);
    emit(e, n->end_capture);
  }
}

static void emit_repeat(Emitter *e, const Node *n, int back) {
  const Node *kid = n->kid;
  if (n->max == 0) {
    return;
  }
  if (n->min == 1 && n->max == 1) {
    emit_node(e, kid, back);
    return;
  }
  if (is_single(kid)) {
    emit(e, RE_REPEAT);
    emit(e, n->min);
    emit(e, n->max);
    emit(e, (uint32_t)n->greedy);
    size_t length_at = e->code.length;
    emit(e, 0);
    uint32_t start = here(e);
    emit_node(e, kid, back);
    e->code.data[length_at] = here(e) - start;
    return;
  }
  int empty = nullable(kid);
  if (!empty && n->min <= 1 && (n->max == 1 || n->max == RE_INFINITY)) {
    if (n->min == 1) {
      emit_reset(e, n);
      emit_node(e, kid, back);
      if (n->max == 1) {
        return;
      }
    }
    uint32_t loop = here(e);
    emit(e, n->greedy ? RE_SPLIT_NEXT : RE_SPLIT_GOTO);
    size_t exit_at = e->code.length;
    emit(e, 0);
    emit_reset(e, n);
    emit_node(e, kid, back);
    if (n->max == RE_INFINITY) {
      emit(e, RE_GOTO);
      emit(e, loop);
    }
    e->code.data[exit_at] = here(e);
    return;
  }
  uint32_t counter = e->registers++;
  uint32_t position = e->registers++;
  emit(e, RE_SET_REGISTER);
  emit(e, counter);
  emit(e, 0);
  uint32_t loop = here(e);
  emit(e, RE_LOOP);
  emit(e, counter);
  emit(e, n->min);
  emit(e, n->max);
  emit(e, (uint32_t)n->greedy);
  size_t body_at = e->code.length;
  emit(e, 0);
  size_t exit_at = e->code.length;
  emit(e, 0);
  e->code.data[body_at] = here(e);
  emit(e, RE_SET_POSITION);
  emit(e, position);
  emit_reset(e, n);
  emit_node(e, kid, back);
  emit(e, RE_LOOP_END);
  emit(e, counter);
  emit(e, empty ? n->min : RE_INFINITY);
  emit(e, position);
  emit(e, loop);
  e->code.data[exit_at] = here(e);
}

static void emit_node(Emitter *e, const Node *n, int back) {
  switch (n->kind) {
  case N_EMPTY:
    return;
  case N_CHAR:
    emit(e, back ? RE_CHAR_BACK : RE_CHAR);
    emit(e, n->c);
    return;
  case N_SET:
    emit_set(e, &n->set, back);
    return;
  case N_SEQ:
    if (back) {
      for (size_t i = n->count; i-- > 0;) {
        emit_node(e, n->kids[i], back);
      }
    } else {
      for (size_t i = 0; i < n->count; ++i) {
        emit_node(e, n->kids[i], back);
      }
    }
    return;
  case N_ALT: {
    Vec ends = {NULL, 0, 0};
    for (size_t i = 0; i < n->count; ++i) {
      size_t split_at = 0;
      if (i + 1 < n->count) {
        emit(e, RE_SPLIT_NEXT);
        split_at = e->code.length;
        emit(e, 0);
      }
      emit_node(e, n->kids[i], back);
      if (i + 1 < n->count) {
        emit(e, RE_GOTO);
        vec_push(&ends, (uint32_t)e->code.length);
        emit(e, 0);
        e->code.data[split_at] = here(e);
      }
    }
    for (size_t i = 0; i < ends.length; ++i) {
      e->code.data[ends.data[i]] = here(e);
    }
    vec_free(&ends);
    return;
  }
  case N_GROUP:
    emit_group(e, n, back);
    return;
  case N_REPEAT:
    emit_repeat(e, n, back);
    return;
  case N_ASSERT: {
    int m = (n->flags & TZ_RE_MULTILINE) != 0;
    int fold = e->unicode && (n->flags & TZ_RE_IGNORE_CASE);
    switch (n->c) {
    case A_LINE_START:
      emit(e, m ? RE_LINE_START_M : RE_LINE_START);
      break;
    case A_LINE_END:
      emit(e, m ? RE_LINE_END_M : RE_LINE_END);
      break;
    case A_WORD_BOUNDARY:
      emit(e, fold ? RE_WORD_BOUNDARY_I : RE_WORD_BOUNDARY);
      break;
    default:
      emit(e, fold ? RE_NOT_WORD_BOUNDARY_I : RE_NOT_WORD_BOUNDARY);
      break;
    }
    return;
  }
  case N_LOOK: {
    emit(e, RE_LOOK);
    emit(e, (uint32_t)n->look);
    size_t next_at = e->code.length;
    emit(e, 0);
    emit_node(e, n->kid, (n->look & RE_LOOK_BEHIND) != 0);
    emit(e, RE_MATCH);
    e->code.data[next_at] = here(e);
    return;
  }
  case N_BACKREF: {
    uint32_t flags = back ? RE_REF_BACK : 0;
    if (n->flags & TZ_RE_IGNORE_CASE) {
      flags |= RE_REF_IGNORE_CASE | (e->unicode ? RE_REF_UNICODE : 0);
    }
    emit(e, RE_BACKREF);
    emit(e, flags);
    emit(e, (uint32_t)n->refs.length);
    for (size_t i = 0; i < n->refs.length; ++i) {
      emit(e, n->refs.data[i]);
    }
    return;
  }
  default:
    return;
  }
}

int tz_re_parse_flags(const uint16_t *text, size_t length, uint32_t *flags) {
  static const char letters[] = "dgimsuvy";
  uint32_t result = 0;
  for (size_t i = 0; i < length; ++i) {
    const char *at = text[i] < 128 && text[i] ? strchr(letters, (int)text[i]) : NULL;
    if (!at) {
      return 0;
    }
    uint32_t bit = 1u << (at - letters);
    if (result & bit) {
      return 0;
    }
    result |= bit;
  }
  if ((result & TZ_RE_UNICODE) && (result & TZ_RE_UNICODE_SETS)) {
    return 0;
  }
  *flags = result;

  return 1;
}

void tz_re_free(uint32_t *program) { re_dealloc(program); }

uint32_t tz_re_name(const uint32_t *program, uint32_t capture, uint32_t *start) {
  uint32_t at = program[TZ_RE_H_NAMES];
  for (uint32_t k = 1; k < capture; ++k) {
    at += 1 + (program[at] + 1) / 2;
  }
  *start = at + 1;

  return program[at];
}

static void escape_source(const uint16_t *pattern, size_t length, Vec *out) {
  if (length == 0) {
    static const char empty[] = "(?:)";
    for (int i = 0; i < 4; ++i) {
      vec_push(out, (uint32_t)empty[i]);
    }
    return;
  }
  int in_class = 0;
  for (size_t i = 0; i < length; ++i) {
    uint32_t c = pattern[i];
    if (c == '\\' && i + 1 < length) {
      vec_push(out, c);
      c = pattern[++i];
      if (!is_line_terminator(c)) {
        vec_push(out, c);
        continue;
      }
      out->length--;
    } else if (c == '[') {
      in_class = 1;
    } else if (c == ']') {
      in_class = 0;
    } else if (c == '/' && !in_class) {
      vec_push(out, '\\');
    }
    const char *escape = c == '\n'     ? "\\n"
                         : c == '\r'   ? "\\r"
                         : c == 0x2028 ? "\\u2028"
                         : c == 0x2029 ? "\\u2029"
                                       : NULL;
    if (escape) {
      for (const char *t = escape; *t; ++t) {
        vec_push(out, (uint32_t)*t);
      }
      continue;
    }
    vec_push(out, c);
  }
}

static void push_units(Vec *program, const uint32_t *units, size_t n) {
  for (size_t i = 0; i < n; i += 2) {
    vec_push(program, (units[i] & 0xFFFF) | (i + 1 < n ? (units[i + 1] & 0xFFFF) << 16 : 0));
  }
}

static int resolve_names(Parser *ps) {
  for (size_t i = 0; i < ps->backref_count; ++i) {
    Node *n = ps->backrefs[i];
    for (uint32_t k = 1; k <= ps->captures; ++k) {
      if (same_name(&ps->names[k], &n->name)) {
        vec_push(&n->refs, k);
      }
    }
    if (n->refs.length == 0) {
      ps->pos = n->at;
      return fail(ps, "Invalid named capture referenced");
    }
  }

  return 1;
}

uint32_t *tz_re_compile(const uint16_t *pattern, size_t length, uint32_t flags, const char **error,
                        size_t *error_at) {
  Parser ps;
  memset(&ps, 0, sizeof ps);
  ps.strict = (flags & TZ_RE_STRICT) != 0;
  flags &= ~(uint32_t)TZ_RE_STRICT;
  ps.p = pattern;
  ps.length = length;
  ps.flags = flags;
  ps.unicode = (flags & (TZ_RE_UNICODE | TZ_RE_UNICODE_SETS)) != 0;
  ps.sets = (flags & TZ_RE_UNICODE_SETS) != 0;
  scan_for_captures(&ps);
  size_t slots = (size_t)ps.total_captures + 2;
  ps.names = (Vec *)re_alloc(slots * sizeof(Vec));
  ps.paths = (Vec *)re_alloc(slots * sizeof(Vec));
  memset(ps.names, 0, slots * sizeof(Vec));
  memset(ps.paths, 0, slots * sizeof(Vec));

  Node *root = parse_disjunction(&ps);
  if (root && !ps.error && !at_end(&ps)) {
    fail(&ps, peek_unit(&ps) == ')' ? "Unmatched ')'" : "Unterminated group");
  }
  if (root && !ps.error) {
    resolve_names(&ps);
  }

  uint32_t *program = NULL;
  if (!ps.error) {
    Emitter e;
    memset(&e, 0, sizeof e);
    e.unicode = ps.unicode;
    e.captures = ps.captures + 1;
    emit_node(&e, root, 0);
    emit(&e, RE_MATCH);

    Vec out = {NULL, 0, 0};
    for (int i = 0; i < TZ_RE_H_COUNT; ++i) {
      vec_push(&out, 0);
    }
    out.data[TZ_RE_H_MAGIC] = TZ_RE_MAGIC;
    out.data[TZ_RE_H_FLAGS] = flags;
    out.data[TZ_RE_H_CAPTURES] = ps.captures + 1;
    out.data[TZ_RE_H_REGISTERS] = e.registers;
    out.data[TZ_RE_H_CODE] = (uint32_t)out.length;
    for (size_t i = 0; i < e.code.length; ++i) {
      vec_push(&out, e.code.data[i]);
    }

    Vec source = {NULL, 0, 0};
    escape_source(pattern, length, &source);
    out.data[TZ_RE_H_SOURCE] = (uint32_t)out.length;
    out.data[TZ_RE_H_SOURCE_LENGTH] = (uint32_t)source.length;
    push_units(&out, source.data, source.length);
    vec_free(&source);

    out.data[TZ_RE_H_NAMES] = (uint32_t)out.length;
    for (uint32_t k = 1; k <= ps.captures; ++k) {
      vec_push(&out, (uint32_t)ps.names[k].length);
      push_units(&out, ps.names[k].data, ps.names[k].length);
    }

    size_t words = (ps.captures + 1 + 31) / 32;
    uint32_t *bits = (uint32_t *)re_alloc(words * sizeof(uint32_t));
    out.data[TZ_RE_H_MANDATORY] = (uint32_t)out.length;
    for (int by_name = 0; by_name < 2; ++by_name) {
      memset(bits, 0, words * sizeof(uint32_t));
      bits[0] = 1;
      mandatory(&ps, root, bits, by_name);
      for (size_t w = 0; w < words; ++w) {
        vec_push(&out, bits[w]);
      }
    }
    re_dealloc(bits);

    uint32_t first[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    int wide = 0;
    uint32_t first_flags = 0;
    if (!nullable(root) && first_units(root, first, &wide, ps.unicode)) {
      first_flags = TZ_RE_FIRST_KNOWN | (wide ? TZ_RE_FIRST_WIDE : 0);
    }
    if (anchored(root)) {
      first_flags |= TZ_RE_FIRST_ANCHORED;
    }
    out.data[TZ_RE_H_FIRST] = (uint32_t)out.length;
    for (int i = 0; i < 8; ++i) {
      vec_push(&out, first[i]);
    }
    vec_push(&out, first_flags);

    out.data[TZ_RE_H_SIZE] = (uint32_t)out.length;
    program = out.data;
    vec_free(&e.code);
  }
  if (ps.error) {
    *error = ps.error;
    *error_at = ps.error_at;
  }
  for (size_t k = 0; k < slots; ++k) {
    vec_free(&ps.names[k]);
    vec_free(&ps.paths[k]);
  }
  re_dealloc(ps.names);
  re_dealloc(ps.paths);
  vec_free(&ps.path);
  vec_free(&ps.open);
  re_dealloc(ps.backrefs);
  free_nodes(&ps);

  return program;
}
