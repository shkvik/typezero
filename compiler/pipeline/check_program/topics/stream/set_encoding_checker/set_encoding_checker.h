#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"

namespace tz {

class SetEncodingChecker {
public:
  explicit SetEncodingChecker(CheckState &owner);

  Expr *checkSetEncoding(CallExpr *call, MemberExpr *member, ClassDecl *cls);
  const Type *encodedStreamType(ClassDecl *cls, llvm::SMLoc loc);

private:
  CheckState &state;
};

}
