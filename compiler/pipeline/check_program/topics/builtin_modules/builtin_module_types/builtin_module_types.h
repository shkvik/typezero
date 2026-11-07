#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include <set>
#include <string>
#include <utility>

namespace tz {

class BuiltinModuleTypes {
public:
  explicit BuiltinModuleTypes(CheckState &owner);

  void declare(llvm::ArrayRef<llvm::StringLiteral> pieces, llvm::StringRef bufferName);
  void exportFrom(NodeModule module, llvm::StringRef name);
  bool declared(llvm::StringRef name) const;
  TypeAliasDecl *alias(llvm::StringRef name) const;
  TypeAliasDecl *exported(NodeModule module, llvm::StringRef name) const;
  const Type *named(llvm::StringRef name);

private:
  CheckState &state;
};

}
