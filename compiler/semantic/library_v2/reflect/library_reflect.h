#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/library/reflect/builtin_constructor_table/builtin_constructor_table.h"
#include "llvm/ADT/StringRef.h"

namespace tz {

class SemanticState;

class LibraryReflect {
public:
  explicit LibraryReflect(SemanticState &owner);

  bool isOnlyChecked() const;


  ClassDecl *builtinConstructorNamed(llvm::StringRef name);
  ClassDecl *builtinConstructor(BuiltinConstructor which);

private:
  bool isCheckOnlyFunction(const FunctionDecl *functionDecl) const;

  SemanticState &state;
};

}
