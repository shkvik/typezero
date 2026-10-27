#ifndef TZ_REGEX_H
#define TZ_REGEX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
  TZ_RE_HAS_INDICES = 1,
  TZ_RE_GLOBAL = 2,
  TZ_RE_IGNORE_CASE = 4,
  TZ_RE_MULTILINE = 8,
  TZ_RE_DOT_ALL = 16,
  TZ_RE_UNICODE = 32,
  TZ_RE_UNICODE_SETS = 64,
  TZ_RE_STICKY = 128,
  TZ_RE_STRICT = 1 << 16,
};

enum {
  TZ_RE_H_MAGIC,
  TZ_RE_H_SIZE,
  TZ_RE_H_FLAGS,
  TZ_RE_H_CAPTURES,
  TZ_RE_H_REGISTERS,
  TZ_RE_H_CODE,
  TZ_RE_H_SOURCE,
  TZ_RE_H_SOURCE_LENGTH,
  TZ_RE_H_NAMES,
  TZ_RE_H_MANDATORY,
  TZ_RE_H_FIRST,
  TZ_RE_H_COUNT,
};

#define TZ_RE_MAGIC 0x45525a54u

enum {
  TZ_RE_FIRST_KNOWN = 1,
  TZ_RE_FIRST_WIDE = 2,
  TZ_RE_FIRST_ANCHORED = 4,
};

enum {
  RE_MATCH,
  RE_FAIL,
  RE_CHAR,
  RE_CHAR_BACK,
  RE_ANY,
  RE_ANY_BACK,
  RE_ALL,
  RE_ALL_BACK,
  RE_SET,
  RE_SET_BACK,
  RE_LINE_START,
  RE_LINE_START_M,
  RE_LINE_END,
  RE_LINE_END_M,
  RE_WORD_BOUNDARY,
  RE_NOT_WORD_BOUNDARY,
  RE_WORD_BOUNDARY_I,
  RE_NOT_WORD_BOUNDARY_I,
  RE_GOTO,
  RE_SPLIT_NEXT,
  RE_SPLIT_GOTO,
  RE_SET_POSITION,
  RE_SAVE,
  RE_SAVE_BACK,
  RE_RESET,
  RE_SET_REGISTER,
  RE_LOOP,
  RE_LOOP_END,
  RE_REPEAT,
  RE_BACKREF,
  RE_LOOK,
};

enum {
  RE_REF_BACK = 1,
  RE_REF_IGNORE_CASE = 2,
  RE_REF_UNICODE = 4,
};

enum {
  RE_LOOK_BEHIND = 1,
  RE_LOOK_NEGATIVE = 2,
};

#define RE_INFINITY 0xFFFFFFFFu

#define TZ_RE_MEMO_PAGE 4096

typedef struct {
  uint8_t **pages;
  int32_t rows, length;
  int64_t *log;
  int32_t log_length, log_capacity;
} TzReMemo;

uint32_t *tz_re_compile(const uint16_t *pattern, size_t length, uint32_t flags, const char **error,
                        size_t *error_at);
void tz_re_free(uint32_t *program);

int tz_re_parse_flags(const uint16_t *text, size_t length, uint32_t *flags);

uint32_t tz_re_canonicalize(uint32_t c, int unicode);

static inline uint32_t tz_re_flags(const uint32_t *program) { return program[TZ_RE_H_FLAGS]; }
static inline uint32_t tz_re_captures(const uint32_t *program) { return program[TZ_RE_H_CAPTURES]; }
static inline int tz_re_mandatory(const uint32_t *program, uint32_t capture) {
  return (program[program[TZ_RE_H_MANDATORY] + capture / 32] >> (capture % 32)) & 1;
}
static inline int tz_re_name_mandatory(const uint32_t *program, uint32_t capture) {
  uint32_t words = (program[TZ_RE_H_CAPTURES] + 31) / 32;

  return (program[program[TZ_RE_H_MANDATORY] + words + capture / 32] >> (capture % 32)) & 1;
}
static inline uint32_t tz_re_source_length(const uint32_t *program) {
  return program[TZ_RE_H_SOURCE_LENGTH];
}
static inline uint16_t tz_re_source_unit(const uint32_t *program, uint32_t i) {
  return (uint16_t)(program[program[TZ_RE_H_SOURCE] + i / 2] >> (16 * (i % 2)));
}
uint32_t tz_re_name(const uint32_t *program, uint32_t capture, uint32_t *start);
static inline uint16_t tz_re_name_unit(const uint32_t *program, uint32_t start, uint32_t i) {
  return (uint16_t)(program[start + (i / 2)] >> (16 * (i % 2)));
}

#ifdef __cplusplus
}
#endif

#ifndef __cplusplus

#include "runtime/core/string/string.h"
#include "runtime/core/tzrt/tzrt.h"

typedef int32_t (*TzReMatch)(const uint32_t *program, const void *units, int32_t wide,
                             int32_t length, int32_t start, int32_t *caps, int32_t sticky,
                             int32_t max);

typedef struct TzRegex {
  int64_t rc;
  const TzLayout *layout;
  int64_t last_index;
  const uint32_t *program;
  uint32_t *owned;
  TzReMatch match;
} TzRegex;

extern const TzLayout tz_re_layout;

typedef struct {
  uint32_t offset;
  uint32_t capture;
  uint32_t optional;
  uint32_t reserved;
} TzMatchName;

typedef struct {
  const TzLayout *layout;
  const TzType *item_type;
  const TzLayout *groups_layout;
  const TzMatchName *names;
  const TzLayout *pair_layout;
  uint32_t size;
  uint32_t items, index, input, groups, indices;
  uint32_t groups_size;
  uint32_t name_count;
  uint32_t pair_size;
  uint32_t reserved;
} TzMatchShape;

extern TzType tz_re_indices_type, tz_re_named_indices_type;

void tz_buf_regex(TzBuf *out, const TzRegex *re);

typedef struct {
  TzRegex *re;
  TzStr input;
  const void *units;
  uint32_t length;
  int32_t two;
  int32_t *matches;
  uint32_t count, capacity, next, captures, copied;
  TzStr *pieces;
  uint32_t piece_count, piece_capacity;
} TzReplacer;

#define TZ_REPLACER_SIZE 128

#endif

#endif
