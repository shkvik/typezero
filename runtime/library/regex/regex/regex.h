// Regular expressions (docs/regex.md): the compiled form of a regex, which
// regex_compile.c produces and regex.c runs.
//
// One parser and compiler serves both ends: tz compiles a regex literal while
// it compiles the program (regex_compile.c is built into tz as well) and puts
// the result into the executable as a constant, and the runtime compiles the
// pattern of `new RegExp(s)` with the same code. A program is an array of
// 32-bit words that holds no pointers: a header (TZ_RE_H_*), the code of a
// backtracking matcher, the source text and what the compiler needs to know
// about the groups.
//
// The matcher follows JavaScript exactly (ECMA-262, RegExp Pattern
// Semantics): the first alternative that leads to a match wins, a quantifier
// resets the groups inside it on each iteration, and an iteration that
// matches nothing ends the loop. Any other matcher (a compiled one) can take
// the place of the interpreter for a regex as long as it gives the same
// results.

#ifndef TZ_REGEX_H
#define TZ_REGEX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Flags, in the order RegExp.prototype.flags lists them.
enum {
  TZ_RE_HAS_INDICES = 1,   // d
  TZ_RE_GLOBAL = 2,        // g
  TZ_RE_IGNORE_CASE = 4,   // i
  TZ_RE_MULTILINE = 8,     // m
  TZ_RE_DOT_ALL = 16,      // s
  TZ_RE_UNICODE = 32,      // u
  TZ_RE_UNICODE_SETS = 64, // v
  TZ_RE_STICKY = 128,      // y
  // Not a flag of the regex: tz's own checks for a literal in the program,
  // errors where JavaScript would quietly do something else than meant
  // (`\p{L}` without u is the text "p{L}"). `new RegExp` goes without them.
  TZ_RE_STRICT = 1 << 16,
};

// The header: word indices into a program.
enum {
  TZ_RE_H_MAGIC, // TZ_RE_MAGIC
  TZ_RE_H_SIZE,  // words in the whole program
  TZ_RE_H_FLAGS,
  TZ_RE_H_CAPTURES,  // groups + 1: the match itself is capture 0
  TZ_RE_H_REGISTERS, // what the code needs besides the captures
  TZ_RE_H_CODE,      // where the code starts
  TZ_RE_H_SOURCE,    // where `source` is: UTF-16, two code units a word
  TZ_RE_H_SOURCE_LENGTH,
  TZ_RE_H_NAMES,     // the names of the groups: for each group, its length
                     // in code units and then the code units, two a word
  TZ_RE_H_MANDATORY, // a bit per capture: it holds a value in every match;
                     // then the same by name: some group of its name does
  TZ_RE_H_FIRST,     // the code units a match may start with (see below)
  TZ_RE_H_COUNT,
};

#define TZ_RE_MAGIC 0x45525a54u // "TZRE"

// The first code units of a match: 256 bits for the units below 256, then a
// word of TZ_RE_FIRST_* flags. Without TZ_RE_FIRST_KNOWN a match may start
// anywhere (it may be empty, for one).
enum {
  TZ_RE_FIRST_KNOWN = 1,
  TZ_RE_FIRST_WIDE = 2,     // also some unit from 256 on
  TZ_RE_FIRST_ANCHORED = 4, // only at the start of the input (`^` without m)
};

// The instructions. Each is an opcode word followed by its operands. Those
// marked "back" match the text before the position, moving it back: the
// body of a lookbehind runs backward.
enum {
  RE_MATCH, // the match succeeded
  RE_FAIL,
  RE_CHAR, // c: one code unit (or BMP code point with u)
  RE_CHAR_BACK,
  RE_ANY, // a character other than a line terminator
  RE_ANY_BACK,
  RE_ALL, // any character
  RE_ALL_BACK,
  RE_SET, // n, 8 words of bits for units below 256, n pairs of ranges
  RE_SET_BACK,
  RE_LINE_START,   // `^`
  RE_LINE_START_M, // `^` with m: also after a line terminator
  RE_LINE_END,
  RE_LINE_END_M,
  RE_WORD_BOUNDARY, // `\b`; `\B` is RE_NOT_WORD_BOUNDARY
  RE_NOT_WORD_BOUNDARY,
  RE_WORD_BOUNDARY_I, // the same with u (or v) and i, where ſ and K are word characters
  RE_NOT_WORD_BOUNDARY_I,
  RE_GOTO,         // target
  RE_SPLIT_NEXT,   // target: tries what follows first, then target
  RE_SPLIT_GOTO,   // target: tries target first, then what follows
  RE_SET_POSITION, // r: register r = position
  RE_SAVE,         // k, r: capture k = register r .. position
  RE_SAVE_BACK,    // k, r: capture k = position .. register r
  RE_RESET,        // from, to: captures from..to-1 become undefined
  RE_SET_REGISTER, // r, value
  RE_LOOP,         // counter, min, max, greedy, body, exit (see regex.c)
  RE_LOOP_END,     // counter, min, position register, loop
  RE_REPEAT,       // min, max, greedy, length: a single character repeated;
                   // its instruction (of `length` words) follows
  RE_BACKREF,      // flags (RE_REF_*), n, captures...: the text of the first
                   // of them that holds a value
  RE_LOOK,         // kind (RE_LOOK_*), next: a lookaround whose body
                   // follows and ends with RE_MATCH; then next
};

enum {
  RE_REF_BACK = 1, // in a lookbehind
  RE_REF_IGNORE_CASE = 2,
  RE_REF_UNICODE = 4, // case folding of u and v
};

enum {
  RE_LOOK_BEHIND = 1,
  RE_LOOK_NEGATIVE = 2,
};

#define RE_INFINITY 0xFFFFFFFFu

// The memo of a matcher that tz compiled
// (compiler/library/regex/codegen/codegen_regex_native.cpp and
// runtime/library/regex/regex/regex.c): a bit for each row, an instruction with
// the state of the counted loops around it, and position, which says that the
// match got there and failed. The bits of TZ_RE_MEMO_PAGE positions make a
// page, row after row; a page is made when the match first gets to it.
#define TZ_RE_MEMO_PAGE 4096

typedef struct {
  uint8_t **pages; // a page per TZ_RE_MEMO_PAGE positions, or NULL
  int32_t rows, length;
  int64_t *log; // what was noted inside lookarounds: position << 32 | row
  int32_t log_length, log_capacity;
} TzReMemo;

// Compiles `pattern` (UTF-16) with `flags`. On success returns the program,
// which tz_re_free releases; on a syntax error returns NULL and sets
// `*error` to the message, as V8 words it ("Unterminated group"), and
// `*error_at` to where in the pattern it is.
uint32_t *tz_re_compile(const uint16_t *pattern, size_t length, uint32_t flags, const char **error,
                        size_t *error_at);
void tz_re_free(uint32_t *program);

// Reads flags such as "gi": returns 0 if a letter is not a flag or repeats,
// or u and v come together.
int tz_re_parse_flags(const uint16_t *text, size_t length, uint32_t *flags);

// The canonical form that /i compares: the same for all the characters of
// one class (case folding with `unicode`, Canonicalize without).
uint32_t tz_re_canonicalize(uint32_t c, int unicode);

// What a program says about itself.
static inline uint32_t tz_re_flags(const uint32_t *program) { return program[TZ_RE_H_FLAGS]; }
static inline uint32_t tz_re_captures(const uint32_t *program) { return program[TZ_RE_H_CAPTURES]; }
static inline int tz_re_mandatory(const uint32_t *program, uint32_t capture) {
  return (program[program[TZ_RE_H_MANDATORY] + capture / 32] >> (capture % 32)) & 1;
}
// Whether the name of group `capture` holds a value in every match: with
// several groups of one name, one of them does.
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
// The name of group `capture` (from 1): its length (0 without a name), and
// where its code units start, for tz_re_name_unit.
uint32_t tz_re_name(const uint32_t *program, uint32_t capture, uint32_t *start);
static inline uint16_t tz_re_name_unit(const uint32_t *program, uint32_t start, uint32_t i) {
  return (uint16_t)(program[start + (i / 2)] >> (16 * (i % 2)));
}

#ifdef __cplusplus
}
#endif

#endif
