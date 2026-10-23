// console.log for arrays and optionals: the part of Node.js's util.inspect
// (lib/internal/util/inspect.js) that the language's values need, ported so
// that programs print exactly what Node prints. Function names follow Node's.
//
// The defaults console.log uses: breakLength 80, compact 3, depth 2,
// maxArrayLength 100, maxStringLength 10000.

#include "tzrt.h"

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
  if (type->kind == TZ_T_OPTIONAL) {
    if (!tz_optional_has(type, value))
      return 0;
    type = type->inner;
  }
  return type->kind == TZ_T_INT || type->kind == TZ_T_LONG || type->kind == TZ_T_DOUBLE;
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

// reduceToSingleString for arrays.
static void reduce_to_single_string(Ctx *ctx, TzBuf *out, Entries *output, int recurse_times,
                                    const TzArray *array) {
  size_t entries = output->count;
  Entries grouped = {NULL, 0, 0};
  Entries *lines = output;
  if (entries > 6 && group_array_elements(ctx, output, array, &grouped))
    lines = &grouped;
  if (ctx->current_depth - recurse_times < kCompact && entries == lines->count) {
    size_t start = lines->count + (size_t)ctx->indentation + 1 + 10;
    int newline = 0;
    for (size_t i = 0; i < lines->count && !newline; ++i)
      newline = contains(lines->items[i].data, lines->items[i].length, '\n');
    if (is_below_break_length(ctx, lines, start) && !newline) {
      tz_buf_ascii(out, "[ ");
      join_entries(out, lines, ", ", 0);
      tz_buf_ascii(out, " ]");
      entries_free(&grouped);
      return;
    }
  }
  // One entry per line.
  tz_buf_push(out, '[');
  tz_buf_push(out, '\n');
  spaces(out, ctx->indentation + 2);
  join_entries(out, lines, ",\n", ctx->indentation + 2);
  tz_buf_push(out, '\n');
  spaces(out, ctx->indentation);
  tz_buf_push(out, ']');
  entries_free(&grouped);
}

// formatRaw and formatArray for arrays.
static void format_array(Ctx *ctx, TzBuf *out, const TzArray *array, int recurse_times) {
  if (array->length == 0) {
    tz_buf_ascii(out, "[]");
    return;
  }
  if (recurse_times > kDepth) {
    tz_buf_ascii(out, "[Array]");
    return;
  }
  ++recurse_times;
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
  reduce_to_single_string(ctx, out, &output, recurse_times, array);
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

static void format_value(Ctx *ctx, TzBuf *out, const TzType *type, const void *value,
                         int recurse_times) {
  switch (type->kind) {
  case TZ_T_FUNCTION:
    format_function(out, (const TzFunction *)value);
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
    format_array(ctx, out, *(TzArray *const *)value, recurse_times);
    return;
  case TZ_T_OPTIONAL:
    if (tz_optional_has(type, value))
      format_value(ctx, out, type->inner, value, recurse_times);
    else
      tz_buf_ascii(out, "undefined");
    return;
  default: // numbers, booleans, undefined
    tz_buf_value(out, type, value, 0);
    return;
  }
}

// console.log of one argument that is not a plain number, boolean or string.
// A string at the top level prints as it is; inside arrays it is quoted.
void tz_print_value(const TzType *type, const void *value) {
  if (type->kind == TZ_T_OPTIONAL) {
    if (!tz_optional_has(type, value)) {
      tz_out_write("undefined", 9);
      return;
    }
    type = type->inner;
  }
  if (type->kind == TZ_T_STRING) {
    const TzStr *s = (const TzStr *)value;
    tz_print_string(s->obj, s->meta);
    return;
  }
  Ctx ctx = {0, 0};
  TzBuf out;
  tz_buf_init(&out);
  format_value(&ctx, &out, type, value, 0);
  tz_out_write_utf16(out.data, out.length);
  tz_buf_free(&out);
}
