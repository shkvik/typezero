#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// Type aliases, interfaces and enums.
class TypeDeclarer {
public:
  explicit TypeDeclarer(Sema &sema);

  // Type aliases and enums (sema_decls.cpp). They are visible in the whole
  // block that declares them, so each block declares them before anything
  // else. Returns those declared; without `resolve` they are resolved later
  // with resolveTypes, as the types of a file may come from other files.
  std::vector<Stmt *> declareTypes(llvm::ArrayRef<Stmt *> stmts, bool resolve = true);
  void resolveTypes(llvm::ArrayRef<Stmt *> declared);
  Stmt *lookupType(llvm::StringRef name) const;
  const Type *resolveAlias(TypeAliasDecl *alias);
  void resolveEnum(EnumDecl *decl);
  Expr *checkEnumMember(EnumDecl *decl, MemberExpr *member, const Type *expected);
  Expr *checkEnumIndex(EnumDecl *decl, IndexExpr *index);

private:
  Sema &sema_;
};

} // namespace tz
