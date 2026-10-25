#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// process, Date.now() and performance.now().
class ProcessChecker {
public:
  explicit ProcessChecker(Sema &sema);

  // process, Date.now() and performance.now() (sema_process.cpp). Each
  // returns null if what it is given is not about them.
  Expr *checkProcessCall(CallExpr *call, MemberExpr *member);
  Expr *checkProcessMember(MemberExpr *member);
  Expr *checkProcessEnvIndex(IndexExpr *index);
  // Assignments to the properties of process, of which only
  // `process.exitCode = code` is allowed.
  Expr *checkProcessAssign(AssignExpr *assign);
  // Whether `expr` is `process.name` of the global process.
  bool isProcessMember(const Expr *expr, llvm::StringRef name) const;
  // The code of `process.exit(code)` or `process.exitCode = code`: an int,
  // or undefined for none. The result is `int | undefined`.
  Expr *checkExitCode(Expr *code);
  // A call of a built-in at `loc` in place of a read such as `process.argv`;
  // `callee` is what it reads.
  Expr *builtinCall(llvm::SMLoc loc, Expr *callee, Builtin builtin, std::vector<Expr *> args,
                    const Type *type);

private:
  Sema &sema_;
};

} // namespace tz
