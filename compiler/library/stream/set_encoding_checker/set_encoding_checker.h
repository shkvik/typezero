#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class SetEncodingChecker {
public:
  explicit SetEncodingChecker(Checker &checker);

  Expr *checkSetEncoding(CallExpr *call, MemberExpr *member, ClassDecl *cls);
  const Type *encodedStreamType(ClassDecl *cls, llvm::SMLoc loc);
  const Type *streamViewOf(const Checker::FieldPath &path, llvm::SMLoc loc) const;
  Expr *checkStreamPipe(CallExpr *call, MemberExpr *member, ClassDecl *cls);

private:
  Checker &checker_;
};

}
