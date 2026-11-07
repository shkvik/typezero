#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include <cstddef>
#include <vector>

namespace tz {

struct Signature {
  std::vector<const Type *> params;
  size_t minArgs = 0;
  const Type *ret = nullptr;
  bool voidResult = false;
  bool pad = true;
  bool asyncVoid = false;
  bool hasThis = false;
};

struct FunctionSnapshot {
  std::vector<FunctionDecl *> functions;
  std::vector<NestedGroup *> groups;
  FuncSymbol *function = nullptr;
  bool chainShortCircuits = false;
  std::vector<ReturnStmt *> *returns = nullptr;
  std::vector<YieldExpr *> *yields = nullptr;
  bool trial = false;
  const FuncSymbol *early = nullptr;
};

}
