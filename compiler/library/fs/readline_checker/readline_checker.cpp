#include "compiler/library/fs/readline_checker/readline_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/expr/call_checker/call_checker.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/library/fs/fs_checker/fs_checker.h"
#include "compiler/library/builtin_modules/runtime_object_checker/runtime_object_checker.h"
#include "compiler/library/process/process_checker/process_checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/SaveAndRestore.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

ReadlineChecker::ReadlineChecker(Sema &sema) : sema_(sema) {}

// createInterface({ input: process.stdin, output: process.stdout }): lines
// of stdin, which `for await` gives one by one; questions are written to the
// output, if there is one. The options are written in the call: input can
// only be stdin.
Expr *ReadlineChecker::checkReadlineCall(CallExpr *call, NodeModule module, llvm::StringRef fn,
                              llvm::SMLoc loc) {
  sema_.file_->usesAsync = true;
  call->type = Type::error();
  auto loosely = [&] {
    for (Expr *&arg : call->args) {
      if (!sema_.processChecker().isProcessMember(arg, "stdin") && !sema_.processChecker().isProcessMember(arg, "stdout") &&
          !sema_.processChecker().isProcessMember(arg, "stderr")) {
        arg = sema_.exprChecker().checkExpr(arg);
      }
    }

    return call;
  };
  if (fn != "createInterface") {
    const char *name = module == NodeModule::Readline ? "node:readline" : "node:readline/promises";
    sema_.diags_.error(loc, "'" + fn + "' of '" + name +
                          "' can only be used as a type; call 'createInterface'");
    return loosely();
  }
  if (sema_.callChecker().rejectSpread(call) || !sema_.argumentChecker().checkArity(call, loc, 1, 2)) {
    return call;
  }
  Expr *input = nullptr, *output = nullptr, *signal = nullptr;
  if (isa<ObjectLitExpr>(call->args[0])) {
    llvm::StringMap<Expr *> options;
    if (!sema_.fsChecker().nodeOptions(call, 0, fn,
                     {"input", "output", "terminal", "crlfDelay", "signal", "historySize",
                      "removeHistoryDuplicates", "prompt", "tabSize", "completer",
                      "escapeCodeTimeout", "history"},
                     options)) {
      return call;
    }
    input = options.lookup("input");
    output = options.lookup("output");
    signal = options.lookup("signal");
    // What concerns a terminal of readline's own (raw mode, history, a
    // completer) tz leaves to the console, which edits the line.
    for (llvm::StringRef ignored : {"terminal", "crlfDelay", "historySize",
                                    "removeHistoryDuplicates", "tabSize", "escapeCodeTimeout"}) {
      if (Expr *value = options.lookup(ignored)) {
        sema_.exprChecker().checkExpr(value);
      }
    }
    for (llvm::StringRef unsupported : {"prompt", "completer", "history"}) {
      if (Expr *value = options.lookup(unsupported)) {
        sema_.diags_.error(value->loc(), "the option '" + unsupported.str() +
                                       "' of 'createInterface' is not supported yet");
        return call;
      }
    }
    if (!input) {
      sema_.diags_.error(call->args[0]->loc(), "'createInterface' needs the input: '{ input: "
                                         "process.stdin }'");
      return call;
    }
  } else {
    input = call->args[0];
    output = call->args.size() > 1 ? call->args[1] : nullptr;
  }
  if (!sema_.processChecker().isProcessMember(input, "stdin")) {
    sema_.diags_.error(input->loc(), "readline reads only stdin yet: the input is 'process.stdin'; for "
                               "the lines of a file, 'readFile' it and split them");
    return call;
  }
  int written = 0;
  if (output && !isa<UndefinedExpr>(output)) {
    if (sema_.processChecker().isProcessMember(output, "stdout")) {
      written = 1;
    } else if (sema_.processChecker().isProcessMember(output, "stderr")) {
      written = 2;
    } else {
      sema_.diags_.error(output->loc(), "the output of readline is 'process.stdout' or "
                                  "'process.stderr'");
      return call;
    }
  }
  auto *where = sema_.ctx_.make<IntLitExpr>(loc, uint64_t(written));
  where->type = Type::intTy();
  call->args = {where, sema_.runtimeObjectChecker().signalOption(signal, loc)};
  call->builtin = Builtin::RlCreate;
  call->type = Type::runtimeObject(
      module == NodeModule::Readline ? Type::Runtime::Readline : Type::Runtime::ReadlinePromises);

  return call;
}

} // namespace tz
