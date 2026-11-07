#include "compiler/pipeline/check_program/topics/os/os_checker/os_checker.h"
#include "compiler/library/builtin_modules/node_check_helpers/node_check_helpers.h"
#include "compiler/pipeline/check_program/topics/expr/argument_checker/argument_checker.h"
#include "compiler/pipeline/check_program/topics/expr/call_checker/call_checker.h"
#include "compiler/pipeline/check_program/topics/expr/expr_checker/expr_checker.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

const Type *OsChecker::cpuInfoType() {
  const Type *d = Type::doubleTy();
  const Type *times = Type::object({{"user", d, false},
                                    {"nice", d, false},
                                    {"sys", d, false},
                                    {"idle", d, false},
                                    {"irq", d, false}});

  return Type::object(
      {{"model", Type::string(), false}, {"speed", d, false}, {"times", times, false}}, "CpuInfo");
}

const Type *OsChecker::userInfoType() {
  return Type::object({{"uid", Type::intTy(), false},
                       {"gid", Type::intTy(), false},
                       {"username", Type::string(), false},
                       {"homedir", Type::string(), false},
                       {"shell", Type::optional(Type::string()), false}},
                      "UserInfo");
}

OsChecker::OsChecker(CheckState &owner)
    : state(owner) {}

Expr *OsChecker::checkOsValue(llvm::StringRef name, Expr *expr) {
  if (name == "EOL") {
    return NodeCheckHelpers::stringLiteral(this->state.getAst(), expr->loc(),
                                           NodeCheckHelpers::targetIsWindows() ? "\r\n" : "\n");
  }
  if (name == "devNull") {
    return NodeCheckHelpers::stringLiteral(this->state.getAst(), expr->loc(),
                                           NodeCheckHelpers::targetIsWindows() ? "\\\\.\\nul"
                                                                               : "/dev/null");
  }

  return nullptr;
}

OsChecker::ResultOsFunction OsChecker::osFunction(llvm::StringRef fn) {
  if (fn == "EOL" || fn == "devNull") {
    std::string message = ErrorTextOs::notCalled(fn);

    return err(OsCheckerOsFunctionError{
        .code = OsCheckerOsFunctionErrorCode::STRING_CALLED,
        .message = message,
    });
  }

  return ok(fn);
}

Expr *OsChecker::checkOsCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc) {
  call->type = Type::error();
  ResultOsFunction function = OsChecker::osFunction(fn);
  if (function.isErr()) {
    this->state.getDiagnostics().error(loc, function.error().message);
    for (Expr *&arg : call->args) {
      arg = ExprChecker(this->state).checkExpr(arg);
    }
    return call;
  }
  bool rejected = CallChecker(this->state).rejectSpread(call);
  ArgumentChecker::ResultArityMatches arity =
      rejected ? ok(call) : ArgumentChecker(this->state).checkArity(call, 0, 0);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
  }
  if (rejected || arity.isErr()) {
    return call;
  }
  auto which = [&](int value) {
    auto *lit = this->state.getAst().make<IntLitExpr>(loc, uint64_t(value));
    lit->type = Type::intTy();
    call->args = {lit};
  };
  static const llvm::StringLiteral strings[] = {"homedir", "tmpdir",  "hostname", "arch",
                                                "type",    "release", "version",  "machine"};
  if (const auto *s = llvm::find(strings, fn); s != std::end(strings)) {
    which(int(s - std::begin(strings)));
    call->target = BuiltinCall{.builtin = Builtin::OsString};
    call->type = Type::string();
    return call;
  }
  static const llvm::StringLiteral numbers[] = {"totalmem", "freemem", "uptime"};
  if (const auto *n = llvm::find(numbers, fn); n != std::end(numbers)) {
    which(int(n - std::begin(numbers)));
    call->target = BuiltinCall{.builtin = Builtin::OsNumber};
    call->type = Type::doubleTy();
    return call;
  }
  if (fn == "platform") {
    call->target = BuiltinCall{.builtin = Builtin::ProcessPlatform};
    call->type = Type::string();
    return call;
  }
  if (fn == "endianness") {
    auto *lit = this->state.getAst().make<StringLitExpr>(loc, u"LE");
    lit->type = Type::string();
    return lit;
  }
  if (fn == "availableParallelism") {
    call->target = BuiltinCall{.builtin = Builtin::OsAvailableParallelism};
    call->type = Type::intTy();
    return call;
  }
  if (fn == "cpus") {
    call->target = BuiltinCall{.builtin = Builtin::OsCpus};
    call->type = Type::array(OsChecker::cpuInfoType());
    return call;
  }
  if (fn == "loadavg") {
    call->target = BuiltinCall{.builtin = Builtin::OsLoadavg};
    call->type = Type::array(Type::doubleTy());
    return call;
  }
  if (fn == "userInfo") {
    call->target = BuiltinCall{.builtin = Builtin::OsUserInfo};
    call->type = OsChecker::userInfoType();
    return call;
  }
  std::string message = ErrorTextOs::notYet(fn);
  this->state.getDiagnostics().error(loc, message);

  return call;
}

}
