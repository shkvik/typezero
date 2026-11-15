#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/StringRef.h"

namespace tz {

class SemanticState;

class LibraryCrypto {
public:
  explicit LibraryCrypto(SemanticState &owner);

  bool isX509Class(const Expr *object) const;
  bool isSubtleClass(const Type *type) const;


  const Type *cryptoTypeNamed(llvm::StringRef name) const;

private:
  SemanticState &state;
};

}
