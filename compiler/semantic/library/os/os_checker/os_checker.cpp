#include "compiler/semantic/library/os/os_checker/os_checker.h"
#include "compiler/program/library/os/os_function_table/os_function_table.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/library/builtin_modules/node_check_helpers/node_check_helpers.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"

namespace tz {

OsChecker::OsChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->callChecker = &all.callChecker;
  this->exprChecker = &all.exprChecker;
  this->argumentChecker = &all.argumentChecker;
}

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
      arg = this->exprChecker->checkExpr(arg);
    }
    return call;
  }
  if (this->hasRejectedArgs(call, loc)) {
    return call;
  }
  std::optional<OsFunction> found = OsFunctionTable::find(fn);
  if (!found.has_value()) {
    return this->reportNotYet(call, fn, loc);
  }

  return this->checkOsFunction(call, *found, loc);
}

bool OsChecker::hasRejectedArgs(CallExpr *call, llvm::SMLoc loc) {
  bool rejected = this->callChecker->rejectSpread(call);
  ArgumentChecker::ResultArityMatches arity =
      rejected ? ok(call) : this->argumentChecker->checkArity(call, 0, 0);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
  }

  return rejected || arity.isErr();
}

Expr *OsChecker::checkOsFunction(CallExpr *call, OsFunction function, llvm::SMLoc loc) {
  switch (function) {
    case OsFunction::HOMEDIR:
      return this->osStringCall(call, loc, 0);
    case OsFunction::TMPDIR:
      return this->osStringCall(call, loc, 1);
    case OsFunction::HOSTNAME:
      return this->osStringCall(call, loc, 2);
    case OsFunction::ARCH:
      return this->osStringCall(call, loc, 3);
    case OsFunction::TYPE:
      return this->osStringCall(call, loc, 4);
    case OsFunction::RELEASE:
      return this->osStringCall(call, loc, 5);
    case OsFunction::VERSION:
      return this->osStringCall(call, loc, 6);
    case OsFunction::MACHINE:
      return this->osStringCall(call, loc, 7);
    case OsFunction::TOTALMEM:
      return this->osNumberCall(call, loc, 0);
    case OsFunction::FREEMEM:
      return this->osNumberCall(call, loc, 1);
    case OsFunction::UPTIME:
      return this->osNumberCall(call, loc, 2);
    case OsFunction::PLATFORM:
      return OsChecker::builtinOsCall(call, Builtin::ProcessPlatform, Type::string());
    case OsFunction::ENDIANNESS:
      return this->endiannessLiteral(loc);
    case OsFunction::AVAILABLE_PARALLELISM:
      return OsChecker::builtinOsCall(call, Builtin::OsAvailableParallelism, Type::intTy());
    case OsFunction::CPUS:
      return OsChecker::builtinOsCall(call, Builtin::OsCpus, Type::array(OsChecker::cpuInfoType()));
    case OsFunction::LOADAVG:
      return OsChecker::builtinOsCall(call, Builtin::OsLoadavg, Type::array(Type::doubleTy()));
    case OsFunction::USER_INFO:
      return OsChecker::builtinOsCall(call, Builtin::OsUserInfo, OsChecker::userInfoType());
  }

  return call;
}

Expr *OsChecker::osStringCall(CallExpr *call, llvm::SMLoc loc, int which) {
  this->setWhichArg(call, loc, which);

  return OsChecker::builtinOsCall(call, Builtin::OsString, Type::string());
}

Expr *OsChecker::osNumberCall(CallExpr *call, llvm::SMLoc loc, int which) {
  this->setWhichArg(call, loc, which);

  return OsChecker::builtinOsCall(call, Builtin::OsNumber, Type::doubleTy());
}

void OsChecker::setWhichArg(CallExpr *call, llvm::SMLoc loc, int which) {
  auto *lit = this->state.getAst().make<IntLitExpr>(loc, uint64_t(which));
  lit->type = Type::intTy();
  call->args = {lit};
}

Expr *OsChecker::builtinOsCall(CallExpr *call, Builtin builtin, const Type *result) {
  call->target = BuiltinCall{.builtin = builtin};
  call->type = result;

  return call;
}

Expr *OsChecker::endiannessLiteral(llvm::SMLoc loc) {
  auto *lit = this->state.getAst().make<StringLitExpr>(loc, u"LE");
  lit->type = Type::string();

  return lit;
}

Expr *OsChecker::reportNotYet(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc) {
  std::string message = ErrorTextOs::notYet(fn);
  this->state.getDiagnostics().error(loc, message);

  return call;
}

}
