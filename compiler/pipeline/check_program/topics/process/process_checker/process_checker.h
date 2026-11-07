#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/process/process_checker/process_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class ProcessChecker {
public:
  explicit ProcessChecker(CheckState &owner);

  Expr *checkProcessCall(CallExpr *call, MemberExpr *member);
  Expr *checkProcessMember(MemberExpr *member);
  Expr *checkProcessEnvIndex(IndexExpr *index);
  Expr *checkProcessAssign(AssignExpr *assign);
  bool isProcessMember(const Expr *expr, llvm::StringRef name) const;
  Expr *checkExitCode(Expr *code);
  Expr *builtinCall(llvm::SMLoc loc, Expr *callee, Builtin builtin, std::vector<Expr *> args,
                    const Type *type);

private:
  using ResultListenedSignal = Result<llvm::StringRef, ProcessCheckerListenedSignalError>;

  static ResultListenedSignal listenedSignal(llvm::StringRef name, const std::u16string &event);
  static std::u16string toUtf16(llvm::StringRef text);

  Expr *checkSignalCall(CallExpr *call, MemberExpr *member);

  CheckState &state;
};

}
