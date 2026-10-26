#pragma once

#include "compiler/overflow/overflow_check/overflow_check.h"

namespace tz {

constexpr unsigned kMaxRounds = 30;

const Expr *stripCasts(const Expr *expr);

std::optional<size_t> callbackArg(Builtin builtin);

Val widenVal(const Val &old, const Val &grown, const Type *type);

bool containsLoop(const Stmt *stmt);

}
