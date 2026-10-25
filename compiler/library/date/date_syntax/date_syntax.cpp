// The parser of date strings that the runtime has for `new Date(s)` and
// Date.parse(s) (runtime/library/date/date_parse/date_parse.c), built into tz
// as well: tz checks the date strings written in a program with it, so a string
// that JavaScript cannot read, or reads as UTC where a time would be local, is
// a compile error (docs/date.md, section 5).

#include "runtime/library/date/date_parse/date_parse.c"
