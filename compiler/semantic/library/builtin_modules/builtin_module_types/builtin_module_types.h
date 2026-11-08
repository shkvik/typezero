#pragma once

#include "compiler/semantic/semantic_state/semantic_state.h"
#include <set>
#include <string>
#include <utility>

namespace tz {

class BuiltinModuleTypes {
public:
  explicit BuiltinModuleTypes(SemanticState &owner);

  void declare(BuiltinText builtin);
  void exportFrom(NodeModule module, llvm::StringRef name);
  bool declared(llvm::StringRef name) const;
  TypeAliasDecl *alias(llvm::StringRef name) const;
  TypeAliasDecl *exported(NodeModule module, llvm::StringRef name) const;
  const Type *named(llvm::StringRef name);

private:
  SemanticState &state;
};

}
