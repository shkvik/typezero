#include "compiler/library/fs/readline_checker/readline_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/library/fs/fs_checker/fs_checker.h"
#include "compiler/library/process/process_checker/process_checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/SaveAndRestore.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

ReadlineChecker::ReadlineChecker(Checker &checker) : checker_(checker) {}

Expr *ReadlineChecker::checkReadlineCall(CallExpr *call, NodeModule module, llvm::StringRef fn,
                                         llvm::SMLoc loc) {
  checker_.file_->usesAsync = true;
  call->type = Type::error();
  auto loosely = [&] {
    for (Expr *&arg : call->args) {
      if (!checker_.processChecker().isProcessMember(arg, "stdin") &&
          !checker_.processChecker().isProcessMember(arg, "stdout") &&
          !checker_.processChecker().isProcessMember(arg, "stderr")) {
        arg = checker_.exprChecker().checkExpr(arg);
      }
    }

    return call;
  };
  if (fn != "createInterface") {
    const char *name = module == NodeModule::Readline ? "node:readline" : "node:readline/promises";
    checker_.diags_.error(loc, "'" + fn + "' of '" + name +
                                   "' can only be used as a type; call 'createInterface'");
    return loosely();
  }
  if (checker_.callChecker().rejectSpread(call) ||
      !checker_.argumentChecker().checkArity(call, loc, 1, 2)) {
    return call;
  }
  Expr *input = nullptr, *output = nullptr;
  if (isa<ObjectLitExpr>(call->args[0])) {
    llvm::StringMap<Expr *> options;
    if (!checker_.fsChecker().nodeOptions(call, 0, fn,
                                          {"input", "output", "terminal", "crlfDelay",
                                           "historySize", "removeHistoryDuplicates", "prompt",
                                           "tabSize", "completer", "escapeCodeTimeout", "history"},
                                          options)) {
      return call;
    }
    input = options.lookup("input");
    output = options.lookup("output");
    for (llvm::StringRef ignored : {"terminal", "crlfDelay", "historySize",
                                    "removeHistoryDuplicates", "tabSize", "escapeCodeTimeout"}) {
      if (Expr *value = options.lookup(ignored)) {
        checker_.exprChecker().checkExpr(value);
      }
    }
    for (llvm::StringRef unsupported : {"prompt", "completer", "history"}) {
      if (Expr *value = options.lookup(unsupported)) {
        checker_.diags_.error(value->loc(), "the option '" + unsupported.str() +
                                                "' of 'createInterface' is not supported yet");
        return call;
      }
    }
    if (!input) {
      checker_.diags_.error(call->args[0]->loc(), "'createInterface' needs the input: '{ input: "
                                                  "process.stdin }'");
      return call;
    }
  } else {
    input = call->args[0];
    output = call->args.size() > 1 ? call->args[1] : nullptr;
  }
  if (!checker_.processChecker().isProcessMember(input, "stdin")) {
    checker_.diags_.error(input->loc(),
                          "readline reads only stdin yet: the input is 'process.stdin'; for "
                          "the lines of a file, 'readFile' it and split them");
    return call;
  }
  int written = 0;
  if (output && !isa<UndefinedExpr>(output)) {
    if (checker_.processChecker().isProcessMember(output, "stdout")) {
      written = 1;
    } else if (checker_.processChecker().isProcessMember(output, "stderr")) {
      written = 2;
    } else {
      checker_.diags_.error(output->loc(), "the output of readline is 'process.stdout' or "
                                           "'process.stderr'");
      return call;
    }
  }
  auto *where = checker_.ctx_.make<IntLitExpr>(loc, uint64_t(written));
  where->type = Type::intTy();
  call->args = {where};
  call->builtin = Builtin::RlCreate;
  call->type = Type::runtimeObject(
      module == NodeModule::Readline ? Type::Runtime::Readline : Type::Runtime::ReadlinePromises);

  return call;
}

}
