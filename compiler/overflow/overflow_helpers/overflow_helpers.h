#pragma once

#include "compiler/overflow/overflow_check/overflow_check.h"

namespace tz {

constexpr unsigned kMaxRounds = 30;

const Expr *stripCasts(const Expr *expr);

// The functions of arrays and maps whose callback runs during the call, and
// the position of the callback among the arguments.
std::optional<size_t> callbackArg(Builtin builtin);

Val widenVal(const Val &old, const Val &grown, const Type *type);

// Whether `stmt` holds a loop, or a callback that runs where it is passed.
bool containsLoop(const Stmt *stmt);

} // namespace tz
