#pragma once

#include "ast/ast.h"
#include "support/diagnostics.h"

namespace tz {

// Warns where int arithmetic may overflow (docs/small.md, section 6). An int
// wraps around at run time, where JavaScript's numbers go on growing, so the
// compiler points at the operations whose values it cannot keep within the
// int range: values from input, files, JSON or parseInt and what is computed
// from them, and counters of loops without a bound. It follows the ranges of
// values through the whole program checked by Sema; the warnings do not stop
// the build.
void checkIntOverflow(SourceFile &program, Diagnostics &diags);

} // namespace tz
