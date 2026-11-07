#include "compiler/pipeline/check_program/topics/fs/readline_checker/readline_checker.h"
#include "compiler/pipeline/check_program/topics/expr/argument_checker/argument_checker.h"
#include "compiler/pipeline/check_program/topics/expr/call_checker/call_checker.h"
#include "compiler/pipeline/check_program/topics/expr/expr_checker/expr_checker.h"
#include "compiler/pipeline/check_program/topics/fs/fs_checker/fs_checker.h"
#include "compiler/pipeline/check_program/topics/process/process_checker/process_checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/SaveAndRestore.h"

using llvm::isa;

namespace tz {

ReadlineChecker::ReadlineChecker(CheckState &owner)
    : state(owner) {}

Expr *ReadlineChecker::checkReadlineCall(CallExpr *call, NodeModule module, llvm::StringRef fn,
                                         llvm::SMLoc loc) {
  this->state.getProgramUse().markUsesAsync();
  call->type = Type::error();
  auto loosely = [&] {
    for (Expr *&arg : call->args) {
      if (!ProcessChecker(this->state).isProcessMember(arg, "stdin") &&
          !ProcessChecker(this->state).isProcessMember(arg, "stdout") &&
          !ProcessChecker(this->state).isProcessMember(arg, "stderr")) {
        arg = ExprChecker(this->state).checkExpr(arg);
      }
    }

    return call;
  };
  if (fn != "createInterface") {
    const char *name = module == NodeModule::Readline ? "node:readline" : "node:readline/promises";
    this->state.getDiagnostics().error(
        loc, "'" + fn + "' of '" + name + "' can only be used as a type; call 'createInterface'");
    return loosely();
  }
  bool rejected = CallChecker(this->state).rejectSpread(call);
  ArgumentChecker::ResultArityMatches arity =
      rejected ? ok(call) : ArgumentChecker(this->state).checkArity(call, 1, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
  }
  if (rejected || arity.isErr()) {
    return call;
  }
  Expr *input = nullptr;
  Expr *output = nullptr;
  if (isa<ObjectLitExpr>(call->args[0])) {
    llvm::StringMap<Expr *> options;
    if (!FsChecker(this->state)
             .nodeOptions(call, 0, fn,
                          {"input", "output", "terminal", "crlfDelay", "historySize",
                           "removeHistoryDuplicates", "prompt", "tabSize", "completer",
                           "escapeCodeTimeout", "history"},
                          options)) {
      return call;
    }
    input = options.lookup("input");
    output = options.lookup("output");
    for (llvm::StringRef ignored : {"terminal", "crlfDelay", "historySize",
                                    "removeHistoryDuplicates", "tabSize", "escapeCodeTimeout"}) {
      if (Expr *value = options.lookup(ignored)) {
        ExprChecker(this->state).checkExpr(value);
      }
    }
    for (llvm::StringRef unsupported : {"prompt", "completer", "history"}) {
      if (Expr *value = options.lookup(unsupported)) {
        this->state.getDiagnostics().error(value->loc(),
                                           "the option '" + unsupported.str() +
                                               "' of 'createInterface' is not supported yet");
        return call;
      }
    }
    if (!input) {
      this->state.getDiagnostics().error(call->args[0]->loc(),
                                         "'createInterface' needs the input: '{ input: "
                                         "process.stdin }'");
      return call;
    }
  } else {
    input = call->args[0];
    output = call->args.size() > 1 ? call->args[1] : nullptr;
  }
  if (!ProcessChecker(this->state).isProcessMember(input, "stdin")) {
    this->state.getDiagnostics().error(
        input->loc(), "readline reads only stdin yet: the input is 'process.stdin'; for "
                      "the lines of a file, 'readFile' it and split them");
    return call;
  }
  int written = 0;
  if (output && !isa<UndefinedExpr>(output)) {
    if (ProcessChecker(this->state).isProcessMember(output, "stdout")) {
      written = 1;
    } else if (ProcessChecker(this->state).isProcessMember(output, "stderr")) {
      written = 2;
    } else {
      this->state.getDiagnostics().error(output->loc(),
                                         "the output of readline is 'process.stdout' or "
                                         "'process.stderr'");
      return call;
    }
  }
  auto *where = this->state.getAst().make<IntLitExpr>(loc, uint64_t(written));
  where->type = Type::intTy();
  call->args = {where};
  call->target = BuiltinCall{.builtin = Builtin::RlCreate};
  call->type = Type::runtimeObject(
      module == NodeModule::Readline ? Type::Runtime::Readline : Type::Runtime::ReadlinePromises);

  return call;
}

}
