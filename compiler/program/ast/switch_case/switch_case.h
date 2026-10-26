#pragma once

#include "compiler/program/ast/expr/expr.h"
#include "compiler/program/ast/stmt/stmt.h"

namespace tz {

class Type;

struct SwitchCase {
  llvm::SMLoc loc;
  Expr *test = nullptr;
  std::vector<Stmt *> body;
  const Type *compareType = nullptr;
};

}
