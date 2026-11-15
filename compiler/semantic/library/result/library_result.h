#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/types/types.h"
#include "compiler/semantic/semantic_state/walk/flow_state_context/flow_state_types.h"
#include <optional>

namespace tz {

class SemanticState;

class LibraryResult {
public:
  explicit LibraryResult(SemanticState &owner);

  void addResultFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const;


  const Type *unifyHalf(const Type *first, const Type *second);
  std::optional<GlobalName> resultName(llvm::StringRef name) const;

private:
  void splitResultFlows(const VarSymbol *tested, Flow &whenOk, Flow &whenErr) const;


  const VarSymbol *testedResult(const CallExpr *test) const;

  SemanticState &state;
};

}
