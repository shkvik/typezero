#pragma once

#include "compiler/semantic/semantic_state/semantic_state.h"
#include <string>

namespace tz {

class SemanticParts;
class BuiltinModuleTypes;

class FsErrors {
public:
  FsErrors(SemanticState &owner, SemanticParts &all);

  const Type *named(llvm::StringRef name);
  const Type *ofCall(const CallExpr *call);

private:
  static std::string textOf(const Expr *expr);
  static bool literalTrue(const Expr *expr);

  SemanticState &state;
  BuiltinModuleTypes *builtinModuleTypes = nullptr;
};

}
