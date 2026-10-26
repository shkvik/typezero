#include "compiler/library/os/os_checker/os_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

const Type *cpuInfoType() {
  const Type *d = Type::doubleTy();
  const Type *times = Type::object({{"user", d, false},
                                    {"nice", d, false},
                                    {"sys", d, false},
                                    {"idle", d, false},
                                    {"irq", d, false}});

  return Type::object(
      {{"model", Type::string(), false}, {"speed", d, false}, {"times", times, false}}, "CpuInfo");
}

const Type *userInfoType() {
  return Type::object({{"uid", Type::intTy(), false},
                       {"gid", Type::intTy(), false},
                       {"username", Type::string(), false},
                       {"homedir", Type::string(), false},
                       {"shell", Type::optional(Type::string()), false}},
                      "UserInfo");
}

}

OsChecker::OsChecker(Checker &checker) : checker_(checker) {}

Expr *OsChecker::checkOsCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc) {
  call->type = Type::error();
  if (fn == "EOL" || fn == "devNull") {
    checker_.diags_.error(loc, "'" + fn + "' is a string, not a function");
    for (Expr *&arg : call->args) {
      arg = checker_.exprChecker().checkExpr(arg);
    }
    return call;
  }
  if (checker_.callChecker().rejectSpread(call) ||
      !checker_.argumentChecker().checkArity(call, loc, 0, 0)) {
    return call;
  }
  auto which = [&](int value) {
    auto *lit = checker_.ctx_.make<IntLitExpr>(loc, uint64_t(value));
    lit->type = Type::intTy();
    call->args = {lit};
  };
  static const llvm::StringLiteral strings[] = {"homedir", "tmpdir",  "hostname", "arch",
                                                "type",    "release", "version",  "machine"};
  if (const auto *s = llvm::find(strings, fn); s != std::end(strings)) {
    which(int(s - std::begin(strings)));
    call->builtin = Builtin::OsString;
    call->type = Type::string();
    return call;
  }
  static const llvm::StringLiteral numbers[] = {"totalmem", "freemem", "uptime"};
  if (const auto *n = llvm::find(numbers, fn); n != std::end(numbers)) {
    which(int(n - std::begin(numbers)));
    call->builtin = Builtin::OsNumber;
    call->type = Type::doubleTy();
    return call;
  }
  if (fn == "platform") {
    call->builtin = Builtin::ProcessPlatform;
    call->type = Type::string();
    return call;
  }
  if (fn == "endianness") {
    auto *lit = checker_.ctx_.make<StringLitExpr>(loc, u"LE");
    lit->type = Type::string();
    return lit;
  }
  if (fn == "availableParallelism") {
    call->builtin = Builtin::OsAvailableParallelism;
    call->type = Type::intTy();
    return call;
  }
  if (fn == "cpus") {
    call->builtin = Builtin::OsCpus;
    call->type = Type::array(cpuInfoType());
    return call;
  }
  if (fn == "loadavg") {
    call->builtin = Builtin::OsLoadavg;
    call->type = Type::array(Type::doubleTy());
    return call;
  }
  if (fn == "userInfo") {
    call->builtin = Builtin::OsUserInfo;
    call->type = userInfoType();
    return call;
  }
  checker_.diags_.error(loc, "'" + fn + "' of 'node:os' is not supported yet");

  return call;
}

}
