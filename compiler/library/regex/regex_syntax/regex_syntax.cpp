// The parser and compiler of regular expressions that the runtime has for
// `new RegExp(s)` (runtime/library/regex/regex_compile/regex_compile.c), built
// into tz as well: tz compiles the regex literals of a program while it
// compiles the program, so a literal's errors are compile errors and its
// compiled form is a constant in the executable.

#define TZ_RE_FOR_COMPILER
#include "runtime/library/regex/regex_compile/regex_compile.c"
