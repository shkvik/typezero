#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class TypeDeclarer {
public:
  explicit TypeDeclarer(Checker &checker);

  std::vector<Stmt *> declareTypes(llvm::ArrayRef<Stmt *> stmts, bool resolve = true);
  void resolveTypes(llvm::ArrayRef<Stmt *> declared);
  Stmt *lookupType(llvm::StringRef name) const;
  const Type *resolveAlias(TypeAliasDecl *alias);
  void resolveEnum(EnumDecl *decl);
  Expr *checkEnumMember(EnumDecl *decl, MemberExpr *member, const Type *expected);
  Expr *checkEnumIndex(EnumDecl *decl, IndexExpr *index);

private:
  Checker &checker_;
};

}
