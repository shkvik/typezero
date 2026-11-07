#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"

namespace tz {

class StreamNewChecker {
public:
  explicit StreamNewChecker(CheckState &owner);

  Expr *checkStreamNew(NewExpr *expr, ClassDecl *generic, const Type *expected);

private:
  CheckState &state;
};

}
