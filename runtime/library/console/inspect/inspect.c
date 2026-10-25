// console.log for arrays, objects, optionals and Results: the part of Node.js's
// util.inspect (lib/internal/util/inspect.js) that the language's values
// need, ported so that programs print exactly what Node prints. Function
// names follow Node's.
//
// The defaults console.log uses: breakLength 80, compact 3, depth 2,
// maxArrayLength 100, maxStringLength 10000.
//
// One difference is deliberate: an optional field that holds undefined is
// not shown, since in tz a missing field and an undefined one are the same.

#include "runtime/core/tzrt/tzrt.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void tz_print_string(void *obj, uint64_t meta);

enum {
  kBreakLength = 80,
  kCompact = 3,
  kDepth = 2,
  kMaxArrayLength = 100,
  kMaxStringLength = 10000,
  kMinLineLength = 16,
};

typedef struct {
  int indentation; // ctx.indentationLvl
  int current_depth;
  // ctx.seen: the arrays and objects being formatted, outermost first. No
  // more than kDepth + 1 are, since deeper ones print as [Object].
  const void *seen[kDepth + 1];
  int seen_count;
  // ctx.circular: the objects that some object inside them refers to; the
  // first is <ref *1>, the next <ref *2>.
  const void **circular;
  int circular_count, circular_capacity;
} Ctx;

// The formatted entries of an array (Node's `output`).
typedef struct {
  TzBuf *items;
  size_t count, capacity;
} Entries;

static TzBuf *entries_add(Entries *e) {
  if (e->count == e->capacity) {
    size_t capacity = e->capacity * 2 + 8;
    TzBuf *items = (TzBuf *)tz_alloc(capacity * sizeof(TzBuf));
    if (e->count)
      memcpy(items, e->items, e->count * sizeof(TzBuf));
    tz_free(e->items);
    e->items = items;
    e->capacity = capacity;
  }
  TzBuf *b = &e->items[e->count++];
  tz_buf_init(b);
  return b;
}

static void entries_free(Entries *e) {
  for (size_t i = 0; i < e->count; ++i)
    tz_buf_free(&e->items[i]);
  tz_free(e->items);
  e->items = NULL;
  e->count = e->capacity = 0;
}

static void spaces(TzBuf *out, int n) {
  for (int i = 0; i < n; ++i)
    tz_buf_push(out, ' ');
}

//===----------------------------------------------------------------------===//
// Display width (getStringWidth with ICU)
//===----------------------------------------------------------------------===//

typedef struct {
  uint32_t first, last;
} Range;

// Control characters, combining marks, format characters and emoji
// modifiers take no column.
static const Range zero_width[] = {
    {0x0000, 0x001F}, {0x007F, 0x009F}, {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD},
    {0x05BF, 0x05BF}, {0x05C1, 0x05C2}, {0x05C4, 0x05C5}, {0x05C7, 0x05C7}, {0x0600, 0x0605},
    {0x0610, 0x061A}, {0x061C, 0x061C}, {0x064B, 0x065F}, {0x0670, 0x0670}, {0x06D6, 0x06DD},
    {0x06DF, 0x06E4}, {0x06E7, 0x06E8}, {0x06EA, 0x06ED}, {0x070F, 0x070F}, {0x0E31, 0x0E31},
    {0x0E34, 0x0E3A}, {0x0E47, 0x0E4E}, {0x180E, 0x180E}, {0x1AB0, 0x1AFF}, {0x1DC0, 0x1DFF},
    {0x200B, 0x200F}, {0x202A, 0x202E}, {0x2060, 0x2064}, {0x2066, 0x206F}, {0x20D0, 0x20FF},
    {0xFE00, 0xFE0F}, {0xFE20, 0xFE2F}, {0xFEFF, 0xFEFF}, {0xFFF9, 0xFFFB}, {0x1F3FB, 0x1F3FF},
    {0xE0001, 0xE0001}, {0xE0020, 0xE007F}, {0xE0100, 0xE01EF},
};

// East Asian Wide and Fullwidth characters take two columns.
static const Range wide[] = {
    {0x1100, 0x115F},   {0x231A, 0x231B},   {0x2329, 0x232A},   {0x23E9, 0x23EC},
    {0x23F0, 0x23F0},   {0x23F3, 0x23F3},   {0x25FD, 0x25FE},   {0x2614, 0x2615},
    {0x2648, 0x2653},   {0x267F, 0x267F},   {0x2693, 0x2693},   {0x26A1, 0x26A1},
    {0x26AA, 0x26AB},   {0x26BD, 0x26BE},   {0x26C4, 0x26C5},   {0x26CE, 0x26CE},
    {0x26D4, 0x26D4},   {0x26EA, 0x26EA},   {0x26F2, 0x26F3},   {0x26F5, 0x26F5},
    {0x26FA, 0x26FA},   {0x26FD, 0x26FD},   {0x2705, 0x2705},   {0x270A, 0x270B},
    {0x2728, 0x2728},   {0x274C, 0x274C},   {0x274E, 0x274E},   {0x2753, 0x2755},
    {0x2757, 0x2757},   {0x2795, 0x2797},   {0x27B0, 0x27B0},   {0x27BF, 0x27BF},
    {0x2B1B, 0x2B1C},   {0x2B50, 0x2B50},   {0x2B55, 0x2B55},   {0x2E80, 0x303E},
    {0x3041, 0x33FF},   {0x3400, 0x4DBF},   {0x4E00, 0x9FFF},   {0xA000, 0xA4CF},
    {0xA960, 0xA97F},   {0xAC00, 0xD7A3},   {0xF900, 0xFAFF},   {0xFE10, 0xFE19},
    {0xFE30, 0xFE6F},   {0xFF00, 0xFF60},   {0xFFE0, 0xFFE6},   {0x16FE0, 0x16FE4},
    {0x17000, 0x18CD5}, {0x1AFF0, 0x1B2FB}, {0x1F004, 0x1F004}, {0x1F0CF, 0x1F0CF},
    {0x1F18E, 0x1F18E}, {0x1F191, 0x1F19A}, {0x1F200, 0x1F251}, {0x1F300, 0x1F320},
    {0x1F32D, 0x1F335}, {0x1F337, 0x1F37C}, {0x1F37E, 0x1F393}, {0x1F3A0, 0x1F3CA},
    {0x1F3CF, 0x1F3D3}, {0x1F3E0, 0x1F3F0}, {0x1F3F4, 0x1F3F4}, {0x1F3F8, 0x1F43E},
    {0x1F440, 0x1F440}, {0x1F442, 0x1F4FC}, {0x1F4FF, 0x1F53D}, {0x1F54B, 0x1F54E},
    {0x1F550, 0x1F567}, {0x1F57A, 0x1F57A}, {0x1F595, 0x1F596}, {0x1F5A4, 0x1F5A4},
    {0x1F5FB, 0x1F64F}, {0x1F680, 0x1F6C5}, {0x1F6CC, 0x1F6CC}, {0x1F6D0, 0x1F6D2},
    {0x1F6D5, 0x1F6D7}, {0x1F6DC, 0x1F6DF}, {0x1F6EB, 0x1F6EC}, {0x1F6F4, 0x1F6FC},
    {0x1F7E0, 0x1F7EB}, {0x1F7F0, 0x1F7F0}, {0x1F90C, 0x1F93A}, {0x1F93C, 0x1F945},
    {0x1F947, 0x1F9FF}, {0x1FA70, 0x1FAFF}, {0x20000, 0x2FFFD}, {0x30000, 0x3FFFD},
};

static int in_ranges(const Range *ranges, size_t count, uint32_t cp) {
  size_t lo = 0, hi = count;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (cp < ranges[mid].first)
      hi = mid;
    else if (cp > ranges[mid].last)
      lo = mid + 1;
    else
      return 1;
  }
  return 0;
}

static size_t string_width(const TzBuf *s) {
  size_t width = 0;
  for (size_t i = 0; i < s->length; ++i) {
    uint32_t cp = s->data[i];
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s->length && s->data[i + 1] >= 0xDC00 &&
        s->data[i + 1] <= 0xDFFF) {
      cp = 0x10000 + ((cp - 0xD800) << 10) + (s->data[i + 1] - 0xDC00);
      ++i;
    }
    if (in_ranges(zero_width, sizeof zero_width / sizeof *zero_width, cp))
      continue;
    width += in_ranges(wide, sizeof wide / sizeof *wide, cp) ? 2 : 1;
  }
  return width;
}

//===----------------------------------------------------------------------===//
// Primitives
//===----------------------------------------------------------------------===//

// Escapes for control characters, the single quote and the backslash.
static const char *const meta[] = {
    "\\x00", "\\x01", "\\x02", "\\x03", "\\x04", "\\x05", "\\x06", "\\x07", "\\b",   "\\t",
    "\\n",   "\\x0B", "\\f",   "\\r",   "\\x0E", "\\x0F", "\\x10", "\\x11", "\\x12", "\\x13",
    "\\x14", "\\x15", "\\x16", "\\x17", "\\x18", "\\x19", "\\x1A", "\\x1B", "\\x1C", "\\x1D",
    "\\x1E", "\\x1F",
};

static int contains(const uint16_t *units, size_t n, uint16_t c) {
  for (size_t i = 0; i < n; ++i)
    if (units[i] == c)
      return 1;
  return 0;
}

// strEscape: quotes and escapes a string. Single quotes by default; double
// quotes if the text has single but no double quotes; backticks if it has
// both but no backtick or `${`.
static void str_escape(TzBuf *out, const uint16_t *units, size_t n) {
  uint16_t quote = '\'';
  if (contains(units, n, '\'')) {
    int dollar_brace = 0;
    for (size_t i = 0; i + 1 < n; ++i)
      dollar_brace |= units[i] == '$' && units[i + 1] == '{';
    if (!contains(units, n, '"'))
      quote = '"';
    else if (!contains(units, n, '`') && !dollar_brace)
      quote = '`';
  }
  tz_buf_push(out, quote);
  for (size_t i = 0; i < n; ++i) {
    uint16_t c = units[i];
    if (c < 0x20) {
      tz_buf_ascii(out, meta[c]);
    } else if (c == '\'' && quote == '\'') {
      tz_buf_ascii(out, "\\'");
    } else if (c == '\\') {
      tz_buf_ascii(out, "\\\\");
    } else if (c >= 0x7F && c <= 0x9F) {
      char text[8];
      static const char hex[] = "0123456789ABCDEF";
      text[0] = '\\';
      text[1] = 'x';
      text[2] = hex[c >> 4];
      text[3] = hex[c & 15];
      tz_buf_ascii_n(out, text, 4);
    } else if (c >= 0xD800 && c <= 0xDFFF) {
      if (c <= 0xDBFF && i + 1 < n && units[i + 1] >= 0xDC00 && units[i + 1] <= 0xDFFF) {
        tz_buf_push(out, c);
        tz_buf_push(out, units[++i]);
        continue;
      }
      // A lone surrogate: \u followed by lowercase hex, as Number#toString(16).
      static const char hex[] = "0123456789abcdef";
      char text[8] = {'\\', 'u', hex[(c >> 12) & 15], hex[(c >> 8) & 15], hex[(c >> 4) & 15],
                      hex[c & 15]};
      tz_buf_ascii_n(out, text, 6);
    } else {
      tz_buf_push(out, c);
    }
  }
  tz_buf_push(out, quote);
}

// formatPrimitive for strings. Long ones are split after line breaks into
// pieces joined by `+`.
static void format_string(Ctx *ctx, TzBuf *out, TzStr s) {
  TzBuf text;
  tz_buf_init(&text);
  tz_buf_str(&text, s);
  size_t length = text.length;
  size_t remaining = 0;
  if (length > kMaxStringLength) {
    remaining = length - kMaxStringLength;
    length = kMaxStringLength;
  }
  if (length > kMinLineLength && (long)length > kBreakLength - ctx->indentation - 4) {
    size_t start = 0;
    for (size_t i = 0; i < length; ++i) {
      if (text.data[i] != '\n' && i + 1 < length)
        continue;
      if (start > 0) {
        tz_buf_ascii(out, " +\n");
        spaces(out, ctx->indentation + 2);
      }
      str_escape(out, text.data + start, i + 1 - start);
      start = i + 1;
    }
  } else {
    str_escape(out, text.data, length);
  }
  if (remaining > 0) {
    char trailer[64];
    int n = snprintf(trailer, sizeof trailer, "... %zu more character%s", remaining,
                     remaining > 1 ? "s" : "");
    tz_buf_ascii_n(out, trailer, (size_t)n);
  }
  tz_buf_free(&text);
}

//===----------------------------------------------------------------------===//
// Arrays
//===----------------------------------------------------------------------===//

static void format_value(Ctx *ctx, TzBuf *out, const TzType *type, const void *value,
                         int recurse_times);

static int is_number_element(const TzType *type, const void *value) {
  type = tz_resolve(type, &value);
  if (type->kind == TZ_T_OPTIONAL) {
    if (!tz_optional_has(type, value))
      return 0;
    type = type->inner;
  }
  if (type->kind == TZ_T_JSON) {
    const TzJson *j = *(TzJson *const *)value;
    return j && j->kind == TZ_JSON_NUMBER;
  }
  // Decimals line up as numbers, as bigints do in Node.
  return type->kind == TZ_T_INT || type->kind == TZ_T_LONG || type->kind == TZ_T_DOUBLE ||
         type->kind == TZ_T_DECIMAL;
}

static void pad(TzBuf *out, const TzBuf *text, const char *suffix, long target, int at_start) {
  long length = (long)text->length + (long)strlen(suffix);
  if (at_start)
    spaces(out, (int)(target - length));
  tz_buf_units(out, text->data, text->length);
  tz_buf_ascii(out, suffix);
  if (!at_start)
    spaces(out, (int)(target - length));
}

// groupArrayElements: lines up more than six short entries in columns.
// Returns 1 and fills `grouped` if it grouped them.
static int group_array_elements(Ctx *ctx, Entries *output, const TzArray *array, Entries *grouped) {
  size_t total_length = 0, max_length = 0;
  size_t output_length = output->count;
  if (kMaxArrayLength < output->count)
    --output_length; // the "... more items" entry is not grouped
  const size_t separator_space = 2;
  size_t *data_len = (size_t *)tz_alloc(output_length * sizeof(size_t));
  for (size_t i = 0; i < output_length; ++i) {
    size_t len = string_width(&output->items[i]);
    data_len[i] = len;
    total_length += len + separator_space;
    if (max_length < len)
      max_length = len;
  }
  size_t actual_max = max_length + separator_space;
  int result = 0;
  if (actual_max * 3 + (size_t)ctx->indentation < kBreakLength &&
      ((double)total_length / (double)actual_max > 5 || max_length <= 6)) {
    double approx_char_heights = 2.5;
    double average_bias = sqrt((double)actual_max - (double)total_length / (double)output->count);
    double biased_max = fmax((double)actual_max - 3 - average_bias, 1);
    double columns_d = floor(sqrt(approx_char_heights * biased_max * (double)output_length) / biased_max + 0.5);
    columns_d = fmin(columns_d, floor((double)(kBreakLength - ctx->indentation) / (double)actual_max));
    columns_d = fmin(columns_d, kCompact * 4);
    columns_d = fmin(columns_d, 15);
    size_t columns = (size_t)columns_d;
    if (columns > 1) {
      size_t *max_line_length = (size_t *)tz_alloc(columns * sizeof(size_t));
      for (size_t i = 0; i < columns; ++i) {
        size_t line_max = 0;
        for (size_t j = i; j < output_length; j += columns)
          if (data_len[j] > line_max)
            line_max = data_len[j];
        max_line_length[i] = line_max + separator_space;
      }
      // Numbers are aligned to the right, everything else to the left.
      int pad_start = 1;
      for (size_t i = 0; i < output->count && i < array->length; ++i)
        if (!is_number_element(array->type, array->data + i * array->type->size))
          pad_start = 0;
      for (size_t i = 0; i < output_length; i += columns) {
        size_t max = i + columns < output_length ? i + columns : output_length;
        TzBuf *line = entries_add(grouped);
        size_t j = i;
        for (; j < max - 1; ++j) {
          long padding = (long)(max_line_length[j - i] + output->items[j].length - data_len[j]);
          pad(line, &output->items[j], ", ", padding, pad_start);
        }
        if (pad_start) {
          long padding = (long)(max_line_length[j - i] + output->items[j].length - data_len[j]) -
                         (long)separator_space;
          pad(line, &output->items[j], "", padding, 1);
        } else {
          tz_buf_units(line, output->items[j].data, output->items[j].length);
        }
      }
      if (kMaxArrayLength < output->count) {
        TzBuf *more = entries_add(grouped);
        tz_buf_units(more, output->items[output_length].data, output->items[output_length].length);
      }
      tz_free(max_line_length);
      result = 1;
    }
  }
  tz_free(data_len);
  return result;
}

static int is_below_break_length(Ctx *ctx, const Entries *output, size_t start) {
  (void)ctx;
  size_t total = output->count + start;
  if (total + output->count > kBreakLength)
    return 0;
  for (size_t i = 0; i < output->count; ++i) {
    total += output->items[i].length;
    if (total > kBreakLength)
      return 0;
  }
  return 1;
}

static void join_entries(TzBuf *out, const Entries *output, const char *separator, int indentation) {
  for (size_t i = 0; i < output->count; ++i) {
    if (i > 0) {
      tz_buf_ascii(out, separator);
      spaces(out, indentation);
    }
    tz_buf_units(out, output->items[i].data, output->items[i].length);
  }
}

// The `<ref *N>` that starts an array or object which something inside it
// refers to (formatRaw's `base`), or "" for others.
static void circular_base(Ctx *ctx, const void *value, char base[32]) {
  base[0] = 0;
  for (int i = 0; i < ctx->circular_count; ++i)
    if (ctx->circular[i] == value)
      snprintf(base, 32, "<ref *%d>", i + 1);
}

// Appends UTF-8 text as UTF-16 code units.
static void buf_utf8(TzBuf *out, const char *text, size_t n);

// The name of an object's class and a space before its brace, as in
// `Point { x: 1 }`; nothing for other objects.
static void class_prefix(TzBuf *out, const TzLayout *layout) {
  if (!layout || !layout->name_length)
    return;
  buf_utf8(out, layout->name, layout->name_length);
  tz_buf_push(out, ' ');
}

// reduceToSingleString for arrays (with `array`, whose entries may be grouped
// in columns) and objects. `open` and `close` are the braces, after the
// class name of `layout` if it has one (`open` may be longer, as
// `[Object: null prototype] {`); `base` is empty or `<ref *N>`.
static void reduce_to_single_string(Ctx *ctx, TzBuf *out, Entries *output, int recurse_times,
                                    const TzArray *array, const TzLayout *layout,
                                    const char *open, char close, const char *base) {
  size_t entries = output->count;
  size_t base_length = strlen(base);
  size_t brace_length =
      strlen(open) + (layout && layout->name_length ? layout->name_length + 1 : 0);
  Entries grouped = {NULL, 0, 0};
  Entries *lines = output;
  if (array && entries > 6 && group_array_elements(ctx, output, array, &grouped))
    lines = &grouped;
  if (ctx->current_depth - recurse_times < kCompact && entries == lines->count) {
    size_t start = lines->count + (size_t)ctx->indentation + brace_length + base_length + 10;
    int newline = 0;
    for (size_t i = 0; i < lines->count && !newline; ++i)
      newline = contains(lines->items[i].data, lines->items[i].length, '\n');
    if (is_below_break_length(ctx, lines, start) && !newline) {
      if (base_length) {
        tz_buf_ascii(out, base);
        tz_buf_push(out, ' ');
      }
      class_prefix(out, layout);
      tz_buf_ascii(out, open);
      tz_buf_push(out, ' ');
      join_entries(out, lines, ", ", 0);
      tz_buf_push(out, ' ');
      tz_buf_push(out, (uint16_t)close);
      entries_free(&grouped);
      return;
    }
  }
  // One entry per line.
  if (base_length) {
    tz_buf_ascii(out, base);
    tz_buf_push(out, ' ');
  }
  class_prefix(out, layout);
  tz_buf_ascii(out, open);
  tz_buf_push(out, '\n');
  spaces(out, ctx->indentation + 2);
  join_entries(out, lines, ",\n", ctx->indentation + 2);
  tz_buf_push(out, '\n');
  spaces(out, ctx->indentation);
  tz_buf_push(out, (uint16_t)close);
  entries_free(&grouped);
}

// formatValue's check for an array or object that contains itself: it
// prints as `[Circular *N]`, and the outer one gets `<ref *N>`.
static int format_circular(Ctx *ctx, TzBuf *out, const void *value) {
  int seen = 0;
  for (int i = 0; i < ctx->seen_count && !seen; ++i)
    seen = ctx->seen[i] == value;
  if (!seen)
    return 0;
  int index = 0;
  for (int i = 0; i < ctx->circular_count && !index; ++i)
    if (ctx->circular[i] == value)
      index = i + 1;
  if (!index) {
    if (ctx->circular_count == ctx->circular_capacity) {
      int capacity = ctx->circular_capacity * 2 + 4;
      const void **items = (const void **)tz_alloc((size_t)capacity * sizeof(void *));
      if (ctx->circular_count)
        memcpy(items, ctx->circular, (size_t)ctx->circular_count * sizeof(void *));
      tz_free((void *)ctx->circular);
      ctx->circular = items;
      ctx->circular_capacity = capacity;
    }
    ctx->circular[ctx->circular_count++] = value;
    index = ctx->circular_count;
  }
  char text[32];
  int n = snprintf(text, sizeof text, "[Circular *%d]", index);
  tz_buf_ascii_n(out, text, (size_t)n);
  return 1;
}

// formatRaw and formatArray for arrays; `extra`, if not NULL, is a property
// shown after the elements (`groups: undefined` of a match's indices).
static void format_array_with(Ctx *ctx, TzBuf *out, const TzArray *array, int recurse_times,
                              const char *extra) {
  if (array->length == 0 && !extra) {
    tz_buf_ascii(out, "[]");
    return;
  }
  if (recurse_times > kDepth) {
    tz_buf_ascii(out, "[Array]");
    return;
  }
  ++recurse_times;
  ctx->seen[ctx->seen_count++] = array;
  ctx->current_depth = recurse_times;

  uint32_t shown = array->length < kMaxArrayLength ? array->length : kMaxArrayLength;
  Entries output = {NULL, 0, 0};
  for (uint32_t i = 0; i < shown; ++i) {
    TzBuf *entry = entries_add(&output);
    ctx->indentation += 2;
    format_value(ctx, entry, array->type, array->data + (size_t)i * array->type->size, recurse_times);
    ctx->indentation -= 2;
  }
  if (array->length > shown) {
    uint32_t remaining = array->length - shown;
    char text[64];
    int n = snprintf(text, sizeof text, "... %u more item%s", remaining, remaining > 1 ? "s" : "");
    tz_buf_ascii_n(entries_add(&output), text, (size_t)n);
  }
  if (extra)
    tz_buf_ascii(entries_add(&output), extra);
  --ctx->seen_count;
  char base[32];
  circular_base(ctx, array, base);
  reduce_to_single_string(ctx, out, &output, recurse_times, array, NULL, "[", ']', base);
  entries_free(&output);
}

// The indices of a match are an array with `groups` too: the positions of
// the named groups, undefined without names (and with names, which tz does
// not keep there, not shown).
static void format_array(Ctx *ctx, TzBuf *out, const TzArray *array, int recurse_times) {
  format_array_with(ctx, out, array, recurse_times,
                    array->type == &tz_re_indices_type ? "groups: undefined" : NULL);
}

//===----------------------------------------------------------------------===//
// Objects
//===----------------------------------------------------------------------===//

static void buf_utf8(TzBuf *out, const char *text, size_t n) {
  for (size_t i = 0; i < n;) {
    uint32_t c = (unsigned char)text[i];
    size_t extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (extra)
      c &= 0x3Fu >> extra;
    for (size_t k = 1; k <= extra && i + k < n; ++k)
      c = (c << 6) | ((unsigned char)text[i + k] & 0x3F);
    i += extra + 1;
    if (c >= 0x10000) {
      c -= 0x10000;
      tz_buf_push(out, (uint16_t)(0xD800 + (c >> 10)));
      tz_buf_push(out, (uint16_t)(0xDC00 + (c & 0x3FF)));
    } else {
      tz_buf_push(out, (uint16_t)c);
    }
  }
}

// A key that matches keyStrRegExp, /^[a-zA-Z_][a-zA-Z_0-9]*$/, is shown as
// it is; any other is quoted like a string.
static int is_plain_key(const char *name, size_t n) {
  if (n == 0)
    return 0;
  for (size_t i = 0; i < n; ++i) {
    char c = name[i];
    int letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    if (!letter && !(i > 0 && c >= '0' && c <= '9'))
      return 0;
  }
  return 1;
}

// formatRaw and formatArray for a tuple, which is an array in JavaScript.
static void format_tuple(Ctx *ctx, TzBuf *out, const TzInstance *tuple, int recurse_times) {
  const TzLayout *layout = tuple->layout;
  if (layout->count == 0) { // `[]`, as an empty array prints, at any depth
    tz_buf_ascii(out, "[]");
    return;
  }
  if (recurse_times > kDepth) {
    tz_buf_ascii(out, "[Array]");
    return;
  }
  // The empty tuple `[]`, as node shows an empty array.
  if (layout->count == 0) {
    tz_buf_ascii(out, "[]");
    return;
  }
  ++recurse_times;
  ctx->seen[ctx->seen_count++] = tuple;
  ctx->current_depth = recurse_times;
  Entries output = {NULL, 0, 0};
  for (uint32_t i = 0; i < layout->count; ++i) {
    TzBuf *entry = entries_add(&output);
    ctx->indentation += 2;
    format_value(ctx, entry, layout->fields[i].type, (const char *)tuple + layout->fields[i].offset,
                 recurse_times);
    ctx->indentation -= 2;
  }
  --ctx->seen_count;
  char base[32];
  circular_base(ctx, tuple, base);
  // Node lines up more than six entries of an array in columns; a tuple has
  // fewer.
  reduce_to_single_string(ctx, out, &output, recurse_times, NULL, NULL, "[", ']', base);
  entries_free(&output);
}

// `name: `, the key of a property.
static void format_key(TzBuf *entry, const TzField *field) {
  if (is_plain_key(field->name, field->name_length)) {
    tz_buf_ascii_n(entry, field->name, field->name_length);
  } else {
    TzBuf key;
    tz_buf_init(&key);
    buf_utf8(&key, field->name, field->name_length);
    str_escape(entry, key.data, key.length);
    tz_buf_free(&key);
  }
  tz_buf_ascii(entry, ": ");
}

// formatRaw for the match of a regex: in JavaScript an array of the
// captures with the properties index, input and groups after them. The first
// field of the layout is that array.
static void format_match(Ctx *ctx, TzBuf *out, const TzInstance *match, int recurse_times) {
  const TzLayout *layout = match->layout;
  const char *fields = (const char *)match;
  if (recurse_times > kDepth) {
    tz_buf_ascii(out, "[Array]");
    return;
  }
  ++recurse_times;
  ctx->seen[ctx->seen_count++] = match;
  ctx->current_depth = recurse_times;
  const TzArray *items = *(TzArray *const *)(fields + layout->fields[0].offset);
  Entries output = {NULL, 0, 0};
  for (uint32_t i = 0; i < items->length; ++i) {
    TzBuf *entry = entries_add(&output);
    ctx->indentation += 2;
    format_value(ctx, entry, items->type, items->data + (size_t)i * items->type->size,
                 recurse_times);
    ctx->indentation -= 2;
  }
  for (uint32_t i = 1; i < layout->count; ++i) {
    const TzField *field = &layout->fields[i];
    TzBuf *entry = entries_add(&output);
    format_key(entry, field);
    ctx->indentation += 2;
    format_value(ctx, entry, field->type, fields + field->offset, recurse_times);
    ctx->indentation -= 2;
  }
  --ctx->seen_count;
  char base[32];
  circular_base(ctx, match, base);
  reduce_to_single_string(ctx, out, &output, recurse_times, items, NULL, "[", ']', base);
  entries_free(&output);
}

static void format_object(Ctx *ctx, TzBuf *out, const TzInstance *object, int recurse_times);

// formatError and formatRaw for an Error: its stack, in brackets if it has
// no places (`[Error: no file]`), every line indented as deep as the error
// is; then what Node shows after it: the fields of a class derived from
// Error, and the cause of an Error made by `new Error(message, { cause })`,
// as in `{ status: 404, [cause]: ... }`. The name is in the stack already.
static void format_error(Ctx *ctx, TzBuf *out, const TzInstance *error, int recurse_times) {
  TzBuf stack;
  tz_buf_init(&stack);
  int places = tz_buf_error_stack(&stack, error);
  TzBuf base;
  tz_buf_init(&base);
  if (!places)
    tz_buf_push(&base, '[');
  for (size_t i = 0; i < stack.length; ++i) {
    tz_buf_push(&base, stack.data[i]);
    if (stack.data[i] == '\n')
      spaces(&base, ctx->indentation);
  }
  if (!places)
    tz_buf_push(&base, ']');
  tz_buf_free(&stack);
  const TzLayout *layout = error->layout;
  const char *fields = (const char *)error;
  const TzInstance *cause = (const TzInstance *)tz_error_cause(error);
  // The keys: the fields that hold a value, as for other objects.
  uint32_t shown = cause != NULL;
  for (uint32_t i = 0; i < layout->count; ++i) {
    const TzField *field = &layout->fields[i];
    shown += !tz_error_base_field(field) &&
             (field->offset == TZ_FIELD_METHOD || field->type->kind != TZ_T_OPTIONAL ||
              tz_optional_has(field->type, fields + field->offset));
  }
  if (!shown) {
    tz_buf_units(out, base.data, base.length);
    tz_buf_free(&base);
    return;
  }
  if (recurse_times > kDepth) {
    tz_buf_push(out, '[');
    buf_utf8(out, layout->name, layout->name_length);
    tz_buf_push(out, ']');
    tz_buf_free(&base);
    return;
  }
  ++recurse_times;
  ctx->seen[ctx->seen_count++] = error;
  ctx->current_depth = recurse_times;
  Entries output = {NULL, 0, 0};
  for (uint32_t i = 0; i < layout->count; ++i) {
    const TzField *field = &layout->fields[i];
    const void *value = fields + field->offset;
    if (tz_error_base_field(field))
      continue;
    int method = field->offset == TZ_FIELD_METHOD;
    if (!method && field->type->kind == TZ_T_OPTIONAL && !tz_optional_has(field->type, value))
      continue;
    TzBuf *entry = entries_add(&output);
    format_key(entry, field);
    if (method) {
      tz_buf_ascii(entry, "[Function: ");
      buf_utf8(entry, field->name, field->name_length);
      tz_buf_push(entry, ']');
      continue;
    }
    ctx->indentation += 2;
    format_value(ctx, entry, field->type, value, recurse_times);
    ctx->indentation -= 2;
  }
  if (cause) {
    TzBuf *entry = entries_add(&output);
    tz_buf_ascii(entry, "[cause]: ");
    ctx->indentation += 2;
    if (!format_circular(ctx, entry, cause))
      format_object(ctx, entry, cause, recurse_times);
    ctx->indentation -= 2;
  }
  --ctx->seen_count;
  // reduceToSingleString with the stack as `base`: one line if it all fits.
  size_t start = output.count + (size_t)ctx->indentation + 1 + base.length + 10;
  int newline = contains(base.data, base.length, '\n');
  for (size_t i = 0; i < output.count && !newline; ++i)
    newline = contains(output.items[i].data, output.items[i].length, '\n');
  int single = ctx->current_depth - recurse_times < kCompact &&
               is_below_break_length(ctx, &output, start) && !newline;
  tz_buf_units(out, base.data, base.length);
  if (single) {
    tz_buf_ascii(out, " { ");
    join_entries(out, &output, ", ", 0);
    tz_buf_ascii(out, " }");
  } else {
    tz_buf_ascii(out, " {\n");
    spaces(out, ctx->indentation + 2);
    join_entries(out, &output, ",\n", ctx->indentation + 2);
    tz_buf_push(out, '\n');
    spaces(out, ctx->indentation);
    tz_buf_push(out, '}');
  }
  entries_free(&output);
  tz_buf_free(&base);
}

// An error nobody caught, as Node prints it (see tz_uncaught in error.c).
void tz_buf_inspect_error(TzBuf *out, const void *error) {
  Ctx ctx;
  memset(&ctx, 0, sizeof ctx);
  format_error(&ctx, out, (const TzInstance *)error, 0);
  tz_free((void *)ctx.circular);
}

static void format_json_key(TzBuf *entry, TzStr key);

// formatRaw with formatMap and formatSet for a Map and a Set: `Map(2) { 'a'
// => 1, 'b' => 2 }`, `Set(1) { 1 }`, and deeper than two levels `[Map]`. A
// Record is a plain object in JavaScript: `{ a: 1, 'b-c': 2 }`, its keys in
// the order they were added (Node puts the keys that are indices first).
static void format_dict(Ctx *ctx, TzBuf *out, const TzMap *m, int recurse_times) {
  const TzMapLayout *l = m->layout;
  char open[48];
  int bare = (m->flags & TZ_MAP_BARE) != 0;
  if (l->kind == TZ_MAP_RECORD)
    snprintf(open, sizeof open, bare ? "[Object: null prototype] {" : "{");
  else
    snprintf(open, sizeof open, "%s(%u) {", l->kind == TZ_MAP_MAP ? "Map" : "Set", m->count);
  if (m->count == 0) {
    tz_buf_ascii(out, open);
    tz_buf_push(out, '}');
    return;
  }
  if (recurse_times > kDepth) {
    tz_buf_ascii(out, l->kind == TZ_MAP_RECORD ? (bare ? "[Object: null prototype]" : "[Object]")
                      : l->kind == TZ_MAP_MAP  ? "[Map]"
                                               : "[Set]");
    return;
  }
  ++recurse_times;
  ctx->seen[ctx->seen_count++] = m;
  ctx->current_depth = recurse_times;
  Entries output = {NULL, 0, 0};
  uint32_t at = 0, shown = 0;
  // A Map and a Set show at most as many entries as an array.
  uint32_t limit = l->kind == TZ_MAP_RECORD ? m->count : kMaxArrayLength;
  ctx->indentation += 2;
  for (const char *e; shown < limit && (e = (const char *)tz_map_next(m, &at)); ++shown) {
    TzBuf *entry = entries_add(&output);
    if (l->kind == TZ_MAP_RECORD) {
      format_json_key(entry, *(const TzStr *)(e + l->key_offset));
      tz_buf_ascii(entry, ": ");
    } else {
      format_value(ctx, entry, l->key, e + l->key_offset, recurse_times);
      if (l->kind == TZ_MAP_MAP)
        tz_buf_ascii(entry, " => ");
    }
    if (l->kind != TZ_MAP_SET)
      format_value(ctx, entry, l->value, e + l->value_offset, recurse_times);
  }
  ctx->indentation -= 2;
  if (m->count > shown) {
    uint32_t remaining = m->count - shown;
    char text[64];
    int n = snprintf(text, sizeof text, "... %u more item%s", remaining, remaining > 1 ? "s" : "");
    tz_buf_ascii_n(entries_add(&output), text, (size_t)n);
  }
  --ctx->seen_count;
  char base[32];
  circular_base(ctx, m, base);
  reduce_to_single_string(ctx, out, &output, recurse_times, NULL, NULL, open, '}', base);
  entries_free(&output);
}

// formatIterator for the iterator of a Map or a Set: what it has left, as
// Node shows it without taking it, `[Map Iterator] { 'a', 'b' }` and
// `[Map Entries] { [ 'a', 1 ] }`. The coroutine keeps the map and its
// position in the promise, which lies after the header and the two pointers
// that start the frame (compiler/lower/codegen/codegen_iter.cpp); the map is
// NULL once the iterator is done.
static void format_dict_iterator(Ctx *ctx, TzBuf *out, const TzInstance *it, int recurse_times) {
  uint32_t flags = it->layout->flags;
  int set = (flags & TZ_LAYOUT_DICT_OF_SET) != 0, entries = (flags & TZ_LAYOUT_DICT_ENTRIES) != 0;
  int values = (flags & TZ_LAYOUT_DICT_VALUES) != 0;
  const char *name = set ? (entries ? "Set Entries" : "Set Iterator")
                         : (entries ? "Map Entries" : "Map Iterator");
  if (recurse_times > kDepth) {
    tz_buf_ascii(out, "[Object [");
    tz_buf_ascii(out, name);
    tz_buf_ascii(out, "]]");
    return;
  }
  const char *promise = (const char *)it + 32;
  const TzMap *m = *(TzMap *const *)(promise + 32);
  uint32_t at = *(const uint32_t *)(promise + 40);
  ++recurse_times;
  ctx->seen[ctx->seen_count++] = it;
  ctx->current_depth = recurse_times;
  Entries output = {NULL, 0, 0};
  uint32_t shown = 0, remaining = 0;
  ctx->indentation += 2;
  for (const char *e; m && (e = (const char *)tz_map_next(m, &at));) {
    if (shown == kMaxArrayLength) {
      ++remaining;
      continue;
    }
    ++shown;
    const TzMapLayout *l = m->layout;
    const char *key = e + l->key_offset;
    const char *value = l->kind == TZ_MAP_SET ? key : e + l->value_offset;
    const TzType *value_type = l->kind == TZ_MAP_SET ? l->key : l->value;
    TzBuf *entry = entries_add(&output);
    if (!entries) {
      if (values)
        format_value(ctx, entry, value_type, value, recurse_times);
      else
        format_value(ctx, entry, l->key, key, recurse_times);
      continue;
    }
    Entries pair = {NULL, 0, 0};
    format_value(ctx, entries_add(&pair), l->key, key, recurse_times);
    format_value(ctx, entries_add(&pair), value_type, value, recurse_times);
    reduce_to_single_string(ctx, entry, &pair, recurse_times, NULL, NULL, "[", ']', "");
    entries_free(&pair);
  }
  ctx->indentation -= 2;
  if (remaining > 0) {
    char text[64];
    int n = snprintf(text, sizeof text, "... %u more item%s", remaining, remaining > 1 ? "s" : "");
    tz_buf_ascii_n(entries_add(&output), text, (size_t)n);
  }
  --ctx->seen_count;
  char open[32];
  snprintf(open, sizeof open, "[%s] {", name);
  char base[32];
  circular_base(ctx, it, base);
  reduce_to_single_string(ctx, out, &output, recurse_times, NULL, NULL, open, '}', base);
  entries_free(&output);
}

// formatPromise: `Promise { 1 }`, `Promise { <pending> }`, `Promise {
// <rejected> Error: ... }`; deeper than two levels `[Promise]`.
static void format_promise(Ctx *ctx, TzBuf *out, const TzPromise *p, int recurse_times) {
  static const TzType error_type = {TZ_T_OBJECT, sizeof(void *), NULL};
  if (recurse_times > kDepth) {
    tz_buf_ascii(out, "[Promise]");
    return;
  }
  ++recurse_times;
  ctx->current_depth = recurse_times;
  Entries output = {NULL, 0, 0};
  TzBuf *entry = entries_add(&output);
  if (p->state == TZ_PROMISE_PENDING) {
    tz_buf_ascii(entry, "<pending>");
  } else {
    if (p->state == TZ_PROMISE_REJECTED)
      tz_buf_ascii(entry, "<rejected> ");
    ctx->indentation += 2;
    if (p->state == TZ_PROMISE_REJECTED)
      format_value(ctx, entry, &error_type, &p->error, recurse_times);
    else if (p->layout->count)
      format_value(ctx, entry, p->layout->fields[0].type, (const char *)p + TZ_PROMISE_VALUE,
                   recurse_times);
    else
      tz_buf_ascii(entry, "undefined");
    ctx->indentation -= 2;
  }
  reduce_to_single_string(ctx, out, &output, recurse_times, NULL, NULL, "Promise {", '}', "");
  entries_free(&output);
}

// An ArrayBuffer: formatArrayBuffer and its hidden byteLength.
static void format_array_buffer(Ctx *ctx, TzBuf *out, const TzArrayBuffer *ab,
                                int recurse_times) {
  if (recurse_times > kDepth) {
    tz_buf_ascii(out, "[ArrayBuffer]");
    return;
  }
  ++recurse_times;
  ctx->current_depth = recurse_times;
  Entries output = {NULL, 0, 0};
  TzBuf *contents = entries_add(&output);
  tz_buf_ascii(contents, "[Uint8Contents]: <");
  tz_buf_bytes_hex(contents, ab->data, ab->length, kMaxArrayLength, "byte");
  tz_buf_push(contents, '>');
  char text[48];
  int n = snprintf(text, sizeof text, "[byteLength]: %d", ab->length);
  tz_buf_ascii_n(entries_add(&output), text, (size_t)n);
  char base[32];
  circular_base(ctx, ab, base);
  reduce_to_single_string(ctx, out, &output, recurse_times, NULL, NULL, "ArrayBuffer {", '}', base);
  entries_free(&output);
}

// Typed arrays (formatTypedArray), Buffer (its own inspect, which shows at
// most 50 bytes at any depth), ArrayBuffer, DataView, and TextEncoder and
// TextDecoder, which show their properties.
static void format_bytes(Ctx *ctx, TzBuf *out, const TzInstance *object, int recurse_times) {
  int32_t kind = tz_bytes_kind(object);
  const TzBytes *b = (const TzBytes *)object;
  char text[64];
  int n;
  switch (kind) {
  case TZ_BYTES_BUFFER:
    tz_buf_ascii(out, "<Buffer ");
    tz_buf_bytes_hex(out, b->data, b->length, 50, "byte");
    tz_buf_push(out, '>');
    return;
  case TZ_BYTES_ARRAY_BUFFER:
    format_array_buffer(ctx, out, (const TzArrayBuffer *)object, recurse_times);
    return;
  case TZ_BYTES_TEXT_ENCODER:
    tz_buf_ascii(out, "{ encoding: 'utf-8' }");
    return;
  case TZ_BYTES_TEXT_DECODER:
    tz_buf_ascii(out, "TextDecoder { encoding: '");
    tz_buf_ascii(out, tz_text_decoder_name(object));
    tz_buf_ascii(out, tz_text_decoder_flag(object, 0) ? "', fatal: true" : "', fatal: false");
    tz_buf_ascii(out, tz_text_decoder_flag(object, 1) ? ", ignoreBOM: true }" : ", ignoreBOM: false }");
    return;
  case TZ_BYTES_DATA_VIEW: {
    if (recurse_times > kDepth) {
      tz_buf_ascii(out, "[DataView]");
      return;
    }
    ++recurse_times;
    ctx->seen[ctx->seen_count++] = object;
    ctx->current_depth = recurse_times;
    Entries output = {NULL, 0, 0};
    n = snprintf(text, sizeof text, "[byteLength]: %d", b->length);
    tz_buf_ascii_n(entries_add(&output), text, (size_t)n);
    n = snprintf(text, sizeof text, "[byteOffset]: %d", b->offset);
    tz_buf_ascii_n(entries_add(&output), text, (size_t)n);
    TzBuf *entry = entries_add(&output);
    tz_buf_ascii(entry, "[buffer]: ");
    ctx->indentation += 2;
    format_array_buffer(ctx, entry, b->buffer, recurse_times);
    ctx->indentation -= 2;
    --ctx->seen_count;
    char base[32];
    circular_base(ctx, object, base);
    reduce_to_single_string(ctx, out, &output, recurse_times, NULL, NULL, "DataView {", '}', base);
    entries_free(&output);
    return;
  }
  default:
    break;
  }
  // A typed array: `Uint8Array(3) [ 1, 2, 3 ]`.
  char open[48];
  snprintf(open, sizeof open, "%s(%d) [", object->layout->name, b->length);
  if (b->length == 0) {
    tz_buf_ascii(out, open);
    tz_buf_push(out, ']');
    return;
  }
  if (recurse_times > kDepth) {
    tz_buf_push(out, '[');
    tz_buf_ascii(out, object->layout->name);
    tz_buf_push(out, ']');
    return;
  }
  ++recurse_times;
  ctx->seen[ctx->seen_count++] = object;
  ctx->current_depth = recurse_times;
  int floating = kind == TZ_BYTES_F16 || kind == TZ_BYTES_F32 || kind == TZ_BYTES_F64;
  int32_t shown = b->length < kMaxArrayLength ? b->length : kMaxArrayLength;
  Entries output = {NULL, 0, 0};
  for (int32_t i = 0; i < shown; ++i) {
    double v = tz_bytes_get(b, i);
    TzBuf *entry = entries_add(&output);
    if (floating && v == 0 && signbit(v)) {
      tz_buf_ascii(entry, "-0");
    } else if (floating) {
      n = (int)tz_format_f64(v, text);
      tz_buf_ascii_n(entry, text, (size_t)n);
    } else {
      n = snprintf(text, sizeof text, "%lld", (long long)v);
      tz_buf_ascii_n(entry, text, (size_t)n);
    }
  }
  if (b->length > shown) {
    int32_t remaining = b->length - shown;
    n = snprintf(text, sizeof text, "... %d more item%s", remaining, remaining > 1 ? "s" : "");
    tz_buf_ascii_n(entries_add(&output), text, (size_t)n);
  }
  --ctx->seen_count;
  // Every element is a number, which groupArrayElements lines up to the
  // right: an array of elements of size 0, which it does not read.
  static const TzType number_type = {TZ_T_INT, 0, NULL};
  TzArray numbers = {0, &number_type, (char *)b->data, (uint32_t)b->length, 0, NULL};
  char base[32];
  circular_base(ctx, object, base);
  reduce_to_single_string(ctx, out, &output, recurse_times, &numbers, NULL, open, ']', base);
  entries_free(&output);
}

// Whether the class of `layout` is `name`.
static int named(const TzLayout *layout, const char *name) {
  size_t n = strlen(name);
  return layout->name_length == n && memcmp(layout->name, name, n) == 0;
}

// An emitter of node:events shows `_maxListeners: undefined`, as node's does.
static int shows_undefined(const TzLayout *layout, const TzField *field) {
  return (layout->flags & TZ_LAYOUT_LIBRARY) && field->name_length == 13 &&
         memcmp(field->name, "_maxListeners", 13) == 0;
}

// The getter of a class of tz's library that gives what the object shows
// as its entries, as node's classes show what they hold
// (TZ_FIELD_GETTER): `__tz_entries`, pairs shown as `'a' => '1'`
// (URLSearchParams), or `__tz_keys`, shown as keys (`'content-type':
// 'text/plain'`, Headers); NULL for any other class.
static const TzField *entries_getter(const TzLayout *layout, int *arrows) {
  for (uint32_t i = 0; i < layout->count; ++i) {
    const TzField *field = &layout->fields[i];
    if (field->offset != TZ_FIELD_GETTER)
      continue;
    if (field->name_length == 12 && memcmp(field->name, "__tz_entries", 12) == 0) {
      *arrows = 1;
      return field;
    }
    if (field->name_length == 9 && memcmp(field->name, "__tz_keys", 9) == 0) {
      *arrows = 0;
      return field;
    }
  }
  return NULL;
}

// An object of such a class: `URLSearchParams { 'a' => '1' }`, from the
// pairs `[string, string][]` its getter gives.
static void format_entries(Ctx *ctx, TzBuf *out, const TzInstance *object, const TzField *field,
                           int arrows, int recurse_times) {
  const TzLayout *layout = object->layout;
  const TzGetterType *getter = (const TzGetterType *)field->type;
  TzArray *pairs = NULL;
  getter->get(object, &pairs);
  if (!pairs || pairs->length == 0) {
    class_prefix(out, layout);
    tz_buf_ascii(out, "{}");
  } else if (recurse_times > kDepth) {
    tz_buf_push(out, '[');
    buf_utf8(out, layout->name, layout->name_length);
    tz_buf_push(out, ']');
  } else {
    ++recurse_times;
    ctx->current_depth = recurse_times;
    Entries output = {NULL, 0, 0};
    const TzType *pair_type = pairs->type;
    ctx->indentation += 2;
    for (uint32_t i = 0; i < pairs->length; ++i) {
      const TzInstance *pair = *(TzInstance *const *)(pairs->data + (size_t)i * pair_type->size);
      const TzField *key = &pair->layout->fields[0], *value = &pair->layout->fields[1];
      const char *base = (const char *)pair;
      TzBuf *entry = entries_add(&output);
      if (arrows) {
        format_value(ctx, entry, key->type, base + key->offset, recurse_times);
        tz_buf_ascii(entry, " => ");
      } else {
        format_json_key(entry, *(const TzStr *)(base + key->offset));
        tz_buf_ascii(entry, ": ");
      }
      format_value(ctx, entry, value->type, base + value->offset, recurse_times);
    }
    ctx->indentation -= 2;
    reduce_to_single_string(ctx, out, &output, recurse_times, NULL, layout, "{", '}', "");
    entries_free(&output);
  }
  if (pairs && tz_rc_dec(&pairs->rc))
    tz_arr_free(pairs);
}

// formatRaw and formatProperty for objects.
static void format_object(Ctx *ctx, TzBuf *out, const TzInstance *object, int recurse_times) {
  const TzLayout *layout = object->layout;
  const char *fields = (const char *)object;
  // The value in the box of an unknown, as it prints itself.
  if (layout->flags & TZ_LAYOUT_BOX) {
    const TzBox *box = (const TzBox *)object;
    format_value(ctx, out, box->type, TZ_BOX_VALUE(box), recurse_times);
    return;
  }
  // A class as a value: `[class Dog extends Animal]`, with its static
  // fields after it, and one of the language, a function in JavaScript:
  // `[Function: String]`. The fields print as an object's, after that name.
  TzLayout titled;
  char title[256];
  int is_class = (layout->flags & TZ_LAYOUT_CLASS) != 0;
  if (is_class) {
    const TzClass *cls = (const TzClass *)object;
    // As node shows a class without a name, and one that extends such a
    // class.
    const TzLayout *parent = cls->parent ? cls->parent->layout : NULL;
    if (parent && !parent->name_length)
      parent = NULL;
    int unnamed = layout->name_length == 0;
    const char *name = unnamed ? "(anonymous)" : layout->name;
    int length = unnamed ? 11 : (int)layout->name_length;
    int n = layout->flags & TZ_LAYOUT_BUILTIN
                ? snprintf(title, sizeof title, "[Function: %.*s]", length, name)
            : parent ? snprintf(title, sizeof title, "[class %.*s extends %.*s]", length, name,
                                (int)parent->name_length, parent->name)
                     : snprintf(title, sizeof title, "[class %.*s]", length, name);
    if (n < 0 || n >= (int)sizeof title)
      n = (int)strlen(title);
    if (layout->count == 0 || recurse_times > kDepth) {
      buf_utf8(out, title, (size_t)n);
      return;
    }
    titled = *layout;
    titled.flags = 0;
    titled.name = title;
    titled.name_length = (uint32_t)n;
    layout = &titled;
  }
  if (layout->flags & TZ_LAYOUT_DICT) {
    format_dict(ctx, out, (const TzMap *)object, recurse_times);
    return;
  }
  if (layout->flags & TZ_LAYOUT_DICT_ITERATOR) {
    format_dict_iterator(ctx, out, object, recurse_times);
    return;
  }
  if (layout->flags & TZ_LAYOUT_TUPLE) {
    format_tuple(ctx, out, object, recurse_times);
    return;
  }
  if (layout->flags & TZ_LAYOUT_PROMISE) {
    format_promise(ctx, out, (const TzPromise *)object, recurse_times);
    return;
  }
  // A timer shows only its kind (Node shows the fields of its own machinery).
  if (layout->flags & TZ_LAYOUT_TIMER) {
    buf_utf8(out, layout->name, layout->name_length);
    tz_buf_ascii(out, " {}");
    return;
  }
  if (layout->flags & TZ_LAYOUT_ERROR) {
    format_error(ctx, out, object, recurse_times);
    return;
  }
  // An iterator shows only what made it, as Node shows one without own
  // properties, at any depth.
  const char *iterator = layout->flags & TZ_LAYOUT_GENERATOR         ? "Generator"
                         : layout->flags & TZ_LAYOUT_ARRAY_ITERATOR  ? "Array Iterator"
                         : layout->flags & TZ_LAYOUT_STRING_ITERATOR ? "String Iterator"
                         : layout->flags & TZ_LAYOUT_ITERATOR_HELPER ? "Iterator Helper"
                         : layout->flags & TZ_LAYOUT_REGEX_ITERATOR  ? "RegExp String Iterator"
                         : layout->flags & TZ_LAYOUT_ASYNC_GENERATOR ? "AsyncGenerator"
                                                                      : NULL;
  if (iterator) {
    tz_buf_ascii(out, "Object [");
    tz_buf_ascii(out, iterator);
    tz_buf_ascii(out, "] {}");
    return;
  }
  if (layout->flags & TZ_LAYOUT_REGEX) {
    tz_buf_regex(out, (const TzRegex *)object);
    return;
  }
  // A date shows its ISO text at any depth, as node shows it.
  if (layout->flags & TZ_LAYOUT_DATE) {
    tz_buf_date(out, ((const TzDate *)object)->time);
    return;
  }
  if (layout->flags & TZ_LAYOUT_DATE_FORMAT) {
    tz_buf_ascii(out, "DateTimeFormat [Intl.DateTimeFormat] {}");
    return;
  }
  if (layout->flags & TZ_LAYOUT_BYTES) {
    format_bytes(ctx, out, object, recurse_times);
    return;
  }
  // The other objects of Intl: `NumberFormat [Intl.NumberFormat] {}`, and
  // what Segmenter's segment() gives as `{}`.
  if (layout->flags & TZ_LAYOUT_INTL) {
    if (layout->name_length) {
      tz_buf_ascii_n(out, layout->name, layout->name_length);
      tz_buf_ascii(out, " [Intl.");
      tz_buf_ascii_n(out, layout->name, layout->name_length);
      tz_buf_ascii(out, "] ");
    }
    tz_buf_ascii(out, "{}");
    return;
  }
  if (layout->flags & TZ_LAYOUT_MATCH) {
    format_match(ctx, out, object, recurse_times);
    return;
  }
  int arrows = 0;
  const TzField *shown_entries = entries_getter(layout, &arrows);
  if (shown_entries) {
    format_entries(ctx, out, object, shown_entries, arrows, recurse_times);
    return;
  }
  // The objects of tz's library that print as node's (TZ_LAYOUT_LIBRARY):
  // the listeners of an event as their array, an async listener as its
  // function, the wrapper of once() as node's bound onceWrapper with its
  // property `listener`.
  if (layout->flags & TZ_LAYOUT_LIBRARY) {
    if ((named(layout, "ListenerList") || named(layout, "ThisListener")) && layout->count > 0) {
      const TzField *field = &layout->fields[0];
      format_value(ctx, out, field->type, fields + field->offset, recurse_times);
      return;
    }
    if (named(layout, "AsyncListener") && layout->count > 0) {
      // `[AsyncFunction: name]`, as node shows an async function.
      const TzField *field = &layout->fields[0];
      TzBuf text;
      tz_buf_init(&text);
      format_value(ctx, &text, field->type, fields + field->offset, recurse_times);
      static const char plain[] = "[Function";
      size_t n = sizeof plain - 1;
      int function = text.length >= n;
      for (size_t i = 0; i < n && function; ++i)
        function = text.data[i] == (uint16_t)plain[i];
      if (function) {
        tz_buf_ascii(out, "[AsyncFunction");
        tz_buf_units(out, text.data + n, text.length - n);
      } else {
        tz_buf_units(out, text.data, text.length);
      }
      tz_buf_free(&text);
      return;
    }
    if (named(layout, "OnceWrapper")) {
      if (recurse_times > kDepth) {
        tz_buf_ascii(out, "[Function]");
        return;
      }
      static const char bound[] = "[Function: bound onceWrapper]";
      titled = *layout;
      titled.name = bound;
      titled.name_length = (uint32_t)(sizeof bound - 1);
      layout = &titled;
    }
  }
  // The groups of a match have no prototype in JavaScript, and every name
  // is a key, even of a group that holds nothing.
  int groups = (layout->flags & TZ_LAYOUT_GROUPS) != 0;
  const char *open = groups ? "[Object: null prototype] {" : "{";
  uint32_t shown = 0;
  for (uint32_t i = 0; i < layout->count; ++i) {
    const TzField *field = &layout->fields[i];
    shown += groups || field->offset == TZ_FIELD_METHOD || field->offset == TZ_FIELD_GETTER ||
             field->type->kind != TZ_T_OPTIONAL ||
             tz_optional_has(field->type, fields + field->offset) || shows_undefined(layout, field);
  }
  if (shown == 0 && is_class) {
    buf_utf8(out, layout->name, layout->name_length);
    return;
  }
  if (shown == 0) {
    class_prefix(out, layout);
    tz_buf_ascii(out, open);
    tz_buf_push(out, '}');
    return;
  }
  if (recurse_times > kDepth) {
    // `[Object]`, or the name of the class: `[Point]`.
    if (groups) {
      tz_buf_ascii(out, "[Object: null prototype]");
    } else {
      tz_buf_push(out, '[');
      if (layout->name_length)
        buf_utf8(out, layout->name, layout->name_length);
      else
        tz_buf_ascii(out, "Object");
      tz_buf_push(out, ']');
    }
    return;
  }
  ++recurse_times;
  // A CryptoKey of Web Crypto shows its fields a level deeper, as node's custom inspect of it
  // does (library/crypto/webcrypto.tz).
  if ((layout->flags & TZ_LAYOUT_LIBRARY) && named(layout, "CryptoKey"))
    ++recurse_times;
  ctx->seen[ctx->seen_count++] = object;
  ctx->current_depth = recurse_times;

  // An object whose keys getters give (a class of tz's library) prints as
  // node's custom inspect of such a class does: its name, then the object
  // of those properties as a fresh inspect formats it, indented from zero.
  int custom = 0;
  for (uint32_t i = 0; i < layout->count && !custom; ++i)
    custom = layout->fields[i].offset == TZ_FIELD_GETTER;
  int outer_indentation = ctx->indentation;
  if (custom)
    ctx->indentation = 0;
  Entries output = {NULL, 0, 0};
  for (uint32_t i = 0; i < layout->count; ++i) {
    const TzField *field = &layout->fields[i];
    if (field->offset == TZ_FIELD_GETTER) {
      // A property of a class of tz's library: what its getter gives,
      // undefined too, as node shows its getters.
      const TzGetterType *getter = (const TzGetterType *)field->type;
      _Alignas(16) char got[64];
      getter->get(object, got);
      TzBuf *entry = entries_add(&output);
      format_key(entry, field);
      ctx->indentation += 2;
      format_value(ctx, entry, getter->result, got, recurse_times);
      ctx->indentation -= 2;
      tz_value_release(getter->result, got);
      continue;
    }
    const void *value = fields + field->offset;
    int method = field->offset == TZ_FIELD_METHOD;
    if (!groups && !method && field->type->kind == TZ_T_OPTIONAL &&
        !tz_optional_has(field->type, value) && !shows_undefined(layout, field))
      continue;
    TzBuf *entry = entries_add(&output);
    if (tz_field_is_symbol(layout, field)) { // `Symbol(kCapture): false`
      buf_utf8(entry, field->name, field->name_length);
      tz_buf_ascii(entry, ": ");
    } else {
      format_key(entry, field);
    }
    if (method) {
      // A method has the name of its key.
      tz_buf_ascii(entry, "[Function: ");
      buf_utf8(entry, field->name, field->name_length);
      tz_buf_push(entry, ']');
      continue;
    }
    ctx->indentation += 2;
    format_value(ctx, entry, field->type, value, recurse_times);
    ctx->indentation -= 2;
  }
  --ctx->seen_count;
  char base[32];
  circular_base(ctx, object, base);
  if (custom) {
    class_prefix(out, layout);
    reduce_to_single_string(ctx, out, &output, recurse_times, NULL, NULL, open, '}', base);
    ctx->indentation = outer_indentation;
  } else {
    reduce_to_single_string(ctx, out, &output, recurse_times, NULL, layout, open, '}', base);
  }
  entries_free(&output);
}

// `[Function: name]`, or `[Function (anonymous)]`.
static void format_function(TzBuf *out, const TzFunction *fn) {
  TzStr name = {fn->info->name.obj, fn->info->name.meta};
  TzBuf text;
  tz_buf_init(&text);
  tz_buf_str(&text, name);
  if (text.length == 0) {
    tz_buf_ascii(out, "[Function (anonymous)]");
  } else {
    tz_buf_ascii(out, "[Function: ");
    tz_buf_units(out, text.data, text.length);
    tz_buf_push(out, ']');
  }
  tz_buf_free(&text);
}

// formatRaw for a Result of neverthrow, which is an instance of its class Ok
// or Err with the one property `value` or `error`: `Ok { value: 84 }`, and
// deeper than two levels `[Ok]`. reduceToSingleString with one entry and the
// brace `Ok {`.
static void format_result(Ctx *ctx, TzBuf *out, const TzResultType *type, const char *value,
                          int recurse_times) {
  int ok = value[type->ok_offset] != 0;
  const char *name = ok ? "Ok" : "Err";
  if (recurse_times > kDepth) {
    tz_buf_push(out, '[');
    tz_buf_ascii(out, name);
    tz_buf_push(out, ']');
    return;
  }
  ++recurse_times;
  ctx->current_depth = recurse_times;
  TzBuf entry;
  tz_buf_init(&entry);
  tz_buf_ascii(&entry, ok ? "value: " : "error: ");
  ctx->indentation += 2;
  if (ok)
    format_value(ctx, &entry, type->base.inner, value + type->value_offset, recurse_times);
  else
    format_value(ctx, &entry, type->error, value + type->error_offset, recurse_times);
  ctx->indentation -= 2;

  int single = 0;
  if (ctx->current_depth - recurse_times < kCompact) {
    size_t start = 1 + (size_t)ctx->indentation + strlen(name) + 2 + 10;
    size_t total = 1 + start;
    single = total + 1 <= kBreakLength && total + entry.length <= kBreakLength &&
             !contains(entry.data, entry.length, '\n');
  }
  tz_buf_ascii(out, name);
  if (single) {
    tz_buf_ascii(out, " { ");
    tz_buf_units(out, entry.data, entry.length);
    tz_buf_ascii(out, " }");
  } else {
    tz_buf_ascii(out, " {\n");
    spaces(out, ctx->indentation + 2);
    tz_buf_units(out, entry.data, entry.length);
    tz_buf_push(out, '\n');
    spaces(out, ctx->indentation);
    tz_buf_push(out, '}');
  }
  tz_buf_free(&entry);
}

// A key of a Json object: as it is when it is an identifier, else quoted;
// `__proto__` is a key of its own there, which Node shows in brackets.
static void format_json_key(TzBuf *entry, TzStr key) {
  TzBuf text;
  tz_buf_init(&text);
  tz_buf_str(&text, key);
  int plain = text.length > 0;
  for (size_t i = 0; i < text.length && plain; ++i) {
    uint16_t c = text.data[i];
    int letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    plain = letter || (i > 0 && c >= '0' && c <= '9');
  }
  static const char proto[] = "__proto__";
  int is_proto = text.length == 9;
  for (size_t i = 0; i < text.length && is_proto; ++i)
    is_proto = text.data[i] == (uint16_t)proto[i];
  if (is_proto)
    tz_buf_ascii(entry, "['__proto__']");
  else if (plain)
    tz_buf_units(entry, text.data, text.length);
  else
    str_escape(entry, text.data, text.length);
  tz_buf_free(&text);
}

// formatRaw for an object that JSON.parse made: its keys in the order
// JavaScript gives them, array indices first.
static void format_json_object(Ctx *ctx, TzBuf *out, const TzJson *object, int recurse_times) {
  uint32_t count = object->u.object.count;
  if (count == 0) {
    tz_buf_ascii(out, "{}");
    return;
  }
  if (recurse_times > kDepth) {
    tz_buf_ascii(out, "[Object]");
    return;
  }
  ++recurse_times;
  ctx->seen[ctx->seen_count++] = object;
  ctx->current_depth = recurse_times;
  uint32_t *order = tz_json_key_order(object);
  Entries output = {NULL, 0, 0};
  for (uint32_t k = 0; k < count; ++k) {
    const TzJsonEntry *e = &object->u.object.entries[order ? order[k] : k];
    TzBuf *entry = entries_add(&output);
    format_json_key(entry, e->key);
    tz_buf_ascii(entry, ": ");
    ctx->indentation += 2;
    format_value(ctx, entry, &tz_json_type, &e->value, recurse_times);
    ctx->indentation -= 2;
  }
  tz_free(order);
  --ctx->seen_count;
  char base[32];
  circular_base(ctx, object, base);
  reduce_to_single_string(ctx, out, &output, recurse_times, NULL, NULL, "{", '}', base);
  entries_free(&output);
}

// A Json value, shown as Node shows what JSON.parse gives.
static void format_json(Ctx *ctx, TzBuf *out, const TzJson *j, int recurse_times) {
  static const TzType double_type = {TZ_T_DOUBLE, sizeof(double), NULL};
  if (!j) {
    tz_buf_ascii(out, "undefined");
    return;
  }
  switch (j->kind) {
  case TZ_JSON_NULL:
    tz_buf_ascii(out, "null");
    return;
  case TZ_JSON_BOOL:
    tz_buf_ascii(out, j->u.boolean ? "true" : "false");
    return;
  case TZ_JSON_NUMBER:
    format_value(ctx, out, &double_type, &j->u.number.value, recurse_times);
    return;
  case TZ_JSON_STRING:
    format_string(ctx, out, j->u.string);
    return;
  case TZ_JSON_ARRAY:
    if (!format_circular(ctx, out, j->u.array))
      format_array(ctx, out, j->u.array, recurse_times);
    return;
  default:
    if (!format_circular(ctx, out, j))
      format_json_object(ctx, out, j, recurse_times);
    return;
  }
}

static void format_value(Ctx *ctx, TzBuf *out, const TzType *type, const void *value,
                         int recurse_times) {
  type = tz_resolve(type, &value);
  switch (type->kind) {
  case TZ_T_JSON:
    format_json(ctx, out, *(TzJson *const *)value, recurse_times);
    return;
  case TZ_T_FUNCTION:
    format_function(out, (const TzFunction *)value);
    return;
  case TZ_T_RESULT:
    format_result(ctx, out, (const TzResultType *)type, (const char *)value, recurse_times);
    return;
  case TZ_T_DOUBLE: {
    double d = *(const double *)value;
    if (d == 0 && signbit(d)) {
      tz_buf_ascii(out, "-0");
      return;
    }
    tz_buf_value(out, type, value, 0);
    return;
  }
  case TZ_T_STRING:
    format_string(ctx, out, *(const TzStr *)value);
    return;
  case TZ_T_ARRAY:
    if (!format_circular(ctx, out, *(void *const *)value))
      format_array(ctx, out, *(TzArray *const *)value, recurse_times);
    return;
  case TZ_T_OBJECT:
    if (!format_circular(ctx, out, *(void *const *)value))
      format_object(ctx, out, *(TzInstance *const *)value, recurse_times);
    return;
  case TZ_T_UNKNOWN: // an object, a box, or undefined
    if (!*(void *const *)value)
      tz_buf_ascii(out, "undefined");
    else if (!format_circular(ctx, out, *(void *const *)value))
      format_object(ctx, out, *(TzInstance *const *)value, recurse_times);
    return;
  case TZ_T_OPTIONAL:
    if (tz_optional_has(type, value))
      format_value(ctx, out, type->inner, value, recurse_times);
    else
      tz_buf_ascii(out, "undefined");
    return;
  default: // numbers, decimals, booleans, undefined
    tz_buf_value(out, type, value, 0);
    return;
  }
}

// console.log of one argument that is not a plain number, boolean or string.
// A string at the top level prints as it is; inside arrays it is quoted.
void tz_print_value(const TzType *type, const void *value) {
  // An unknown prints as what it holds.
  if (type->kind == TZ_T_UNKNOWN) {
    const TzInstance *u = *(const TzInstance *const *)value;
    if (!u) {
      tz_out_write("undefined", 9);
      return;
    }
    if (u->layout->flags & TZ_LAYOUT_BOX) {
      const TzBox *box = (const TzBox *)u;
      tz_print_value(box->type, TZ_BOX_VALUE(box));
      return;
    }
  }
  if (type->kind == TZ_T_OPTIONAL) {
    if (!tz_optional_has(type, value)) {
      tz_out_write("undefined", 9);
      return;
    }
    type = type->inner;
  }
  type = tz_resolve(type, &value);
  if (type->kind == TZ_T_STRING) {
    const TzStr *s = (const TzStr *)value;
    tz_print_string(s->obj, s->meta);
    return;
  }
  if (type->kind == TZ_T_JSON) { // a Json string prints as a string does
    const TzJson *j = *(TzJson *const *)value;
    if (j && j->kind == TZ_JSON_STRING) {
      tz_print_string(j->u.string.obj, j->u.string.meta);
      return;
    }
  }
  Ctx ctx;
  memset(&ctx, 0, sizeof ctx);
  TzBuf out;
  tz_buf_init(&out);
  format_value(&ctx, &out, type, value, 0);
  tz_out_write_utf16(out.data, out.length);
  tz_buf_free(&out);
  tz_free((void *)ctx.circular);
}

// util.inspect of an unknown, for the messages of tz's library (library/):
// a string is quoted, as in an array.
void tz_lib_inspect(TzStr *out, const void *u, TzMaker make) {
  (void)make;
  static const TzType unknown_type = {TZ_T_UNKNOWN, sizeof(void *), NULL};
  Ctx ctx;
  memset(&ctx, 0, sizeof ctx);
  TzBuf text;
  tz_buf_init(&text);
  format_value(&ctx, &text, &unknown_type, &u, 0);
  tz_free((void *)ctx.circular);
  *out = tz_buf_take(&text);
}
