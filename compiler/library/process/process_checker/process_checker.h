#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class ProcessChecker {
public:
  explicit ProcessChecker(Checker &owner);

  Expr *checkProcessCall(CallExpr *call, MemberExpr *member);
  Expr *checkProcessMember(MemberExpr *member);
  Expr *checkProcessEnvIndex(IndexExpr *index);
  Expr *checkProcessAssign(AssignExpr *assign);
  bool isProcessMember(const Expr *expr, llvm::StringRef name) const;
  Expr *checkExitCode(Expr *code);
  Expr *builtinCall(llvm::SMLoc loc, Expr *callee, Builtin builtin, std::vector<Expr *> args,
                    const Type *type);

private:
  static std::u16string toUtf16(llvm::StringRef text);

  Expr *checkSignalCall(CallExpr *call, MemberExpr *member);

  Checker &checker;
};

}
