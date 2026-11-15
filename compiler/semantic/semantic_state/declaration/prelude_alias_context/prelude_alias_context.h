#pragma once

#include "compiler/program/ast/ast.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

namespace tz {

class PreludeAliasContext {
public:
  void rememberBuiltinAlias(TypeAliasDecl *alias);


  TypeAliasDecl *findBuiltinAlias(llvm::StringRef name) const;

private:
  llvm::StringMap<TypeAliasDecl *> builtinAliases;
};

}
