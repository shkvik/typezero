// JSON.parse.

#include "runtime/library/json/json/json.h"
#include "runtime/library/json/json_values/json_values.h"
#include "runtime/core/alloc/alloc.h"
#include "runtime/core/array/array.h"
#include "runtime/core/error/error.h"
#include "runtime/core/parallel/parallel.h"
#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"
#include <math.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// An iterative parser with the grammar, the errors and the messages of V8's
// JsonParser, so that a bad text fails as it does in node. Positions count
// UTF-16 code units.

enum {
  T_STRING,
  T_NUMBER,
  T_LBRACE,
  T_RBRACE,
  T_LBRACK,
  T_RBRACK,
  T_TRUE,
  T_FALSE,
  T_NULL,
  T_COLON,
  T_COMMA,
  T_WHITESPACE,
  T_ILLEGAL,
  T_EOS,
};

// An object or an array that is being read, and for an object the key whose
// value comes next.
typedef struct {
  TzJson *container;
  TzStr key;
  int has_key;
} Frame;

typedef struct {
  TzBuf text; // the code units of the whole text
  const uint16_t *s;
  uint32_t n, pos;
  Frame *frames;
  size_t depth, capacity;
  // What an error must release besides the frames: a value that waits to be
  // stored, the text of a string being read.
  TzJson *pending;
  TzBuf scratch;
  // Where an error goes back to (__builtin_setjmp): tz_json_parse.
  void *fail[5];
} Parser;

static int32_t current(const Parser *p) { return p->pos < p->n ? p->s[p->pos] : -1; }

static int is_digit(int32_t c) { return c >= '0' && c <= '9'; }

static int token_of(int32_t c) {
  if (c < 0) {
    return T_EOS;
  }
  if (is_digit(c) || c == '-') {
    return T_NUMBER;
  }
  switch (c) {
  case '"':
    return T_STRING;
  case '{':
    return T_LBRACE;
  case '}':
    return T_RBRACE;
  case '[':
    return T_LBRACK;
  case ']':
    return T_RBRACK;
  case 't':
    return T_TRUE;
  case 'f':
    return T_FALSE;
  case 'n':
    return T_NULL;
  case ':':
    return T_COLON;
  case ',':
    return T_COMMA;
  case ' ':
  case '\t':
  case '\n':
  case '\r':
    return T_WHITESPACE;
  default:
    return T_ILLEGAL;
  }
}

static void skip_whitespace(Parser *p) {
  while (p->pos < p->n && token_of(p->s[p->pos]) == T_WHITESPACE) {
    ++p->pos;
  }
}

static void parser_release(Parser *p) {
  for (size_t i = 0; i < p->depth; ++i) {
    if (p->frames[i].has_key) {
      tz_str_release_value(p->frames[i].key);
    }
    tz_json_release(p->frames[i].container);
  }
  tz_free(p->frames);
  p->frames = NULL;
  p->depth = 0;
  tz_json_release(p->pending);
  p->pending = NULL;
  tz_buf_free(&p->scratch);
}

// " in JSON at position 5 (line 1 column 6)": V8 counts lines at \n, \r and
// \r\n, the line terminators of JSON.
static void at_position(TzBuf *b, const Parser *p) {
  uint32_t line = 1, line_start = 0;
  for (uint32_t i = 0; i < p->pos; ++i) {
    if (p->s[i] == '\r' && i + 1 < p->pos && p->s[i + 1] == '\n') {
      ++i;
    }
    if (p->s[i] == '\r' || p->s[i] == '\n') {
      ++line;
      line_start = i + 1;
    }
  }
  char text[96];
  int n = snprintf(text, sizeof text, " at position %u (line %u column %u)", p->pos, line,
                   p->pos - line_start + 1);
  tz_buf_ascii_n(b, text, (size_t)n);
}

static int is_special(const Parser *p) {
  static const char *const words[] = {"[object Object]", "undefined", "Infinity", "NaN"};
  for (size_t w = 0; w < sizeof words / sizeof *words; ++w) {
    size_t length = strlen(words[w]);
    if (length != p->n) {
      continue;
    }
    size_t i = 0;
    while (i < length && p->s[i] == (uint16_t)words[w][i]) {
      ++i;
    }
    if (i == length) {
      return 1;
    }
  }

  return 0;
}

// V8's ReportUnexpectedToken: the token at the current position does not
// fit. `message` replaces the general one about the token, with the
// position; without it, a token that is not a number or a string is shown
// with the text around it.
TZ_NORETURN static void unexpected(Parser *p, int token, const char *message) {
  parser_release(p);
  TzBuf b;
  tz_buf_init(&b);
  if (message) {
    tz_buf_ascii(&b, message);
    at_position(&b, p);
  } else if (token == T_EOS) {
    tz_buf_ascii(&b, "Unexpected end of JSON input");
  } else if (token == T_NUMBER || token == T_STRING) {
    tz_buf_ascii(&b, token == T_NUMBER ? "Unexpected number in JSON" : "Unexpected string in JSON");
    at_position(&b, p);
  } else if (is_special(p)) {
    tz_buf_push(&b, '"');
    tz_buf_units(&b, p->s, p->n);
    tz_buf_ascii(&b, "\" is not valid JSON");
  } else {
    tz_buf_ascii(&b, "Unexpected token '");
    tz_buf_push(&b, p->s[p->pos]);
    tz_buf_ascii(&b, "', ");
    // Ten code units on each side of a long text.
    uint32_t from = 0, to = p->n;
    const char *before = "", *after = "";
    if (p->n >= 21) {
      if (p->pos < 10) {
        to = p->pos + 10;
        after = "...";
      } else if (p->pos < p->n - 10) {
        from = p->pos - 10;
        to = p->pos + 10;
        before = after = "...";
      } else {
        from = p->pos - 10;
        before = "...";
      }
    }
    tz_buf_ascii(&b, before);
    tz_buf_push(&b, '"');
    tz_buf_units(&b, p->s + from, to - from);
    tz_buf_push(&b, '"');
    tz_buf_ascii(&b, after);
    tz_buf_ascii(&b, " is not valid JSON");
  }
  tz_buf_free(&p->text);
  tz_json_throw(TZ_SYNTAX_ERROR, &b);
  __builtin_longjmp(p->fail, 1);
}

TZ_NORETURN static void unexpected_here(Parser *p) { unexpected(p, token_of(current(p)), NULL); }

static void scan_literal(Parser *p, const char *word) {
  ++p->pos; // the first letter chose the literal
  for (size_t i = 1; word[i]; ++i, ++p->pos) {
    if (p->pos >= p->n) {
      unexpected(p, T_EOS, NULL);
    }
    if (p->s[p->pos] != (uint16_t)word[i]) {
      unexpected_here(p);
    }
  }
}

static int hex_value(int32_t c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }

  return -1;
}

// A string, after its opening quote; returns an owned string.
static TzStr scan_string(Parser *p) {
  uint32_t start = p->pos;
  for (;;) { // the usual string, without escapes, is taken as it is
    if (p->pos >= p->n) {
      unexpected(p, T_ILLEGAL, "Unterminated string in JSON");
    }
    uint16_t c = p->s[p->pos];
    if (c == '"') {
      TzStr s = tz_str_from_utf16(p->s + start, p->pos - start);
      ++p->pos;
      return s;
    }
    if (c == '\\') {
      break;
    }
    if (c < 0x20) {
      unexpected(p, T_ILLEGAL, "Bad control character in string literal in JSON");
    }
    ++p->pos;
  }
  TzBuf *b = &p->scratch;
  b->length = 0;
  tz_buf_units(b, p->s + start, p->pos - start);
  for (;;) {
    if (p->pos >= p->n) {
      unexpected(p, T_ILLEGAL, "Unterminated string in JSON");
    }
    uint16_t c = p->s[p->pos];
    if (c == '"') {
      ++p->pos;
      return tz_str_from_utf16(b->data, b->length);
    }
    if (c < 0x20) {
      unexpected(p, T_ILLEGAL, "Bad control character in string literal in JSON");
    }
    if (c != '\\') {
      tz_buf_push(b, c);
      ++p->pos;
      continue;
    }
    if (++p->pos >= p->n) {
      unexpected(p, T_EOS, NULL);
    }
    c = p->s[p->pos];
    switch (c) {
    case '"':
    case '\\':
    case '/':
      tz_buf_push(b, c);
      break;
    case 'b':
      tz_buf_push(b, '\b');
      break;
    case 'f':
      tz_buf_push(b, '\f');
      break;
    case 'n':
      tz_buf_push(b, '\n');
      break;
    case 'r':
      tz_buf_push(b, '\r');
      break;
    case 't':
      tz_buf_push(b, '\t');
      break;
    case 'u': {
      uint32_t value = 0;
      for (int k = 0; k < 4; ++k) {
        ++p->pos;
        int digit = hex_value(current(p));
        if (digit < 0) {
          unexpected(p, T_ILLEGAL, "Bad Unicode escape in JSON");
        }
        value = value * 16 + (uint32_t)digit;
      }
      tz_buf_push(b, (uint16_t)value);
      break;
    }
    default:
      unexpected(p, T_ILLEGAL, "Bad escaped character in JSON");
    }
    ++p->pos;
  }
}

static TzJson *parse_number(Parser *p) {
  uint32_t start = p->pos;
  if (current(p) == '-') {
    ++p->pos;
  }
  if (current(p) == '0') {
    // A zero is the whole integer part: "01" is not a number.
    ++p->pos;
    if (is_digit(current(p))) {
      unexpected(p, T_NUMBER, NULL);
    }
  } else {
    if (!is_digit(current(p))) {
      unexpected(p, T_ILLEGAL, "No number after minus sign in JSON");
    }
    while (is_digit(current(p))) {
      ++p->pos;
    }
  }
  if (current(p) == '.') {
    ++p->pos;
    if (!is_digit(current(p))) {
      unexpected(p, T_ILLEGAL, "Unterminated fractional number in JSON");
    }
    while (is_digit(current(p))) {
      ++p->pos;
    }
  }
  if (current(p) == 'e' || current(p) == 'E') {
    ++p->pos;
    if (current(p) == '-' || current(p) == '+') {
      ++p->pos;
    }
    if (!is_digit(current(p))) {
      unexpected(p, T_ILLEGAL, "Exponent part is missing a number in JSON");
    }
    while (is_digit(current(p))) {
      ++p->pos;
    }
  }
  uint32_t length = p->pos - start;
  char local[64];
  char *text = length < sizeof local ? local : (char *)tz_alloc(length + 1);
  uint32_t digits = 0, nonzero = 0;
  int exponent = 0;
  for (uint32_t i = 0; i < length; ++i) {
    char c = (char)p->s[start + i];
    text[i] = c;
    exponent |= c == 'e' || c == 'E';
    if (!exponent && is_digit(c)) {
      ++digits;
      nonzero |= c != '0';
    }
  }
  text[length] = 0;
  double value = strtod(text, NULL); // correctly rounded, as JavaScript requires
  // The text stays where the double cannot give its digits back: more than
  // 15 of them, or beyond the range of a double. JSON.parse<T> reads a
  // decimal or a long from it exactly.
  int keep = digits > 15 || isinf(value) || (value == 0 && nonzero);
  TzJson *j;
  if (keep) {
    j = tz_json_node_new(TZ_JSON_NUMBER, sizeof(j->u.number));
    j->flags |= F_TEXT;
    j->u.number.text = tz_str_from_ascii(text, length);
  } else {
    j = tz_json_node_new(TZ_JSON_NUMBER, sizeof(double));
  }
  j->u.number.value = value;
  if (text != local) {
    tz_free(text);
  }

  return j;
}

static void push_frame(Parser *p, TzJson *container) {
  if (p->depth == p->capacity) {
    size_t capacity = p->capacity * 2 + 16;
    Frame *frames = (Frame *)tz_alloc(capacity * sizeof(Frame));
    if (p->depth) {
      memcpy(frames, p->frames, p->depth * sizeof(Frame));
    }
    tz_free(p->frames);
    p->frames = frames;
    p->capacity = capacity;
  }
  Frame *f = &p->frames[p->depth++];
  f->container = container;
  f->has_key = 0;
}

// A key and its colon, in the object of the innermost frame.
static void read_key(Parser *p, const char *message) {
  skip_whitespace(p);
  if (current(p) != '"') {
    unexpected(p, token_of(current(p)), message);
  }
  ++p->pos;
  Frame *f = &p->frames[p->depth - 1];
  f->key = scan_string(p);
  f->has_key = 1;
  skip_whitespace(p);
  if (current(p) != ':') {
    unexpected(p, token_of(current(p)), "Expected ':' after property name in JSON");
  }
  ++p->pos;
}

static TzJson *parse(Parser *p) {
  for (;;) {
    TzJson *value;
    // A value, or the start of an array or an object.
    skip_whitespace(p);
    switch (token_of(current(p))) {
    case T_STRING:
      ++p->pos;
      value = tz_json_string_node(scan_string(p));
      break;
    case T_NUMBER:
      value = parse_number(p);
      break;
    case T_LBRACE:
      ++p->pos;
      skip_whitespace(p);
      if (current(p) == '}') {
        ++p->pos;
        value = tz_json_object_new();
        break;
      }
      push_frame(p, tz_json_object_new());
      read_key(p, "Expected property name or '}' in JSON");
      continue;
    case T_LBRACK:
      ++p->pos;
      skip_whitespace(p);
      if (current(p) == ']') {
        ++p->pos;
        value = tz_json_array_node(0);
        break;
      }
      push_frame(p, tz_json_array_node(4));
      continue;
    case T_TRUE:
      scan_literal(p, "true");
      value = tz_json_retained(&tz_json_true_node);
      break;
    case T_FALSE:
      scan_literal(p, "false");
      value = tz_json_retained(&tz_json_false_node);
      break;
    case T_NULL:
      scan_literal(p, "null");
      value = tz_json_retained(&tz_json_null_node);
      break;
    default:
      unexpected_here(p);
    }

    // The value goes into the containers it completes.
    for (;;) {
      if (p->depth == 0) {
        skip_whitespace(p);
        if (p->pos < p->n) {
          p->pending = value;
          unexpected(p, token_of(current(p)), "Unexpected non-whitespace character after JSON");
        }
        return value;
      }
      Frame *f = &p->frames[p->depth - 1];
      TzJson *container = f->container;
      if (container->kind == TZ_JSON_OBJECT) {
        tz_json_put(container, f->key, value);
        tz_str_release_value(f->key);
        f->has_key = 0;
        skip_whitespace(p);
        if (current(p) == ',') {
          ++p->pos;
          read_key(p, "Expected double-quoted property name in JSON");
          break;
        }
        --p->depth;
        if (current(p) != '}') {
          p->pending = container;
          unexpected(p, token_of(current(p)), "Expected ',' or '}' after property value in JSON");
        }
      } else {
        tz_arr_push_value(container->u.array, &value);
        skip_whitespace(p);
        if (current(p) == ',') {
          ++p->pos;
          break;
        }
        --p->depth;
        if (current(p) != ']') {
          p->pending = container;
          unexpected(p, token_of(current(p)), "Expected ',' or ']' after array element in JSON");
        }
      }
      ++p->pos;
      value = container;
    }
  }
}

// JSON.parse(text). A text that is not JSON is a SyntaxError, and NULL:
// unexpected released what the parser had and came back here.
TzJson *tz_json_parse(void *obj, uint64_t meta) {
  TzStr str = {obj, meta};
  Parser p;
  memset(&p, 0, sizeof p);
  if (__builtin_setjmp(p.fail)) {
    return NULL;
  }
  tz_buf_init(&p.text);
  tz_buf_str(&p.text, str);
  p.s = p.text.data;
  p.n = (uint32_t)p.text.length;
  tz_buf_init(&p.scratch);
  TzJson *result = parse(&p);
  tz_free(p.frames);
  tz_buf_free(&p.scratch);
  tz_buf_free(&p.text);

  return result;
}

// The values of embedJson, kept for the leak check: the address of each
// cache the program filled.
typedef struct {
  TzJson **slots[64];
  TzJson ***more;
  size_t count, capacity;
} Caches;

Caches tz_json_caches; // not static: see the rules in tzrt.h

// embedJson(path): the text of the file, checked when the program was
// compiled, is read the first time the program gets there; `cache` keeps
// the value for the next times, so the same file gives the same value, as
// require() of a JSON file in node does.
// Threads fill a cache once, under the lock, with a value they share
// (publish.c).
TzLock tz_json_embed_lock;

TzJson *tz_json_embed(TzJson **cache, void *obj, uint64_t meta) {
  TzJson *found = __atomic_load_n(cache, __ATOMIC_ACQUIRE);
  if (found) {
    return tz_json_retained(found);
  }
  tz_lock(&tz_json_embed_lock);
  if (!*cache) {
    TzJson *parsed = tz_json_parse(obj, meta);
#if TZ_PARALLEL
    tz_json_publish(parsed);
#endif
    __atomic_store_n(cache, parsed, __ATOMIC_RELEASE);
    Caches *c = &tz_json_caches;
    if (c->count < 64) {
      c->slots[c->count] = cache;
    } else {
      size_t extra = c->count - 64;
      if (extra == c->capacity) {
        size_t capacity = c->capacity * 2 + 16;
        TzJson ***more = (TzJson ***)tz_alloc(capacity * sizeof(TzJson **));
        if (extra) {
          memcpy(more, c->more, extra * sizeof(TzJson **));
        }
        tz_free(c->more);
        c->more = more;
        c->capacity = capacity;
      }
      c->more[extra] = cache;
    }
    ++c->count;
  }
  found = *cache;
  tz_unlock(&tz_json_embed_lock);

  return tz_json_retained(found);
}

void tz_json_shutdown(void) {
  Caches *c = &tz_json_caches;
  for (size_t i = 0; i < c->count; ++i) {
    TzJson **cache = i < 64 ? c->slots[i] : c->more[i - 64];
    TzJson *j = *cache;
    *cache = NULL;
    tz_json_release(j);
  }
  tz_free(c->more);
  memset(c, 0, sizeof *c);
}
