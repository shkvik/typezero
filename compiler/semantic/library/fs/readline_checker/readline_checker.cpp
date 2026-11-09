#include "compiler/semantic/library/fs/readline_checker/readline_checker.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/library/fs/fs_checker/fs_checker.h"
#include "compiler/semantic/library/process/process_checker/process_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/SaveAndRestore.h"

using llvm::isa;

namespace tz {

ReadlineChecker::ReadlineChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->fsChecker = &all.getFsChecker();
  this->callChecker = &all.getCallChecker();
  this->exprChecker = &all.getExprChecker();
  this->processChecker = &all.getProcessChecker();
  this->argumentChecker = &all.getArgumentChecker();
}

ReadlineChecker::ResultReadlineUsed ReadlineChecker::readlineFunction(llvm::StringRef fn,
                                                                      NodeModule module) {
  if (fn != "createInterface") {
    std::string message = ErrorTextReadline::typeOnly(fn, module != NodeModule::Readline);

    return err(ReadlineCheckerReadlineFunctionError{
        .code = ReadlineCheckerReadlineFunctionErrorCode::TYPE_ONLY,
        .message = message,
    });
  }

  return ok(fn);
}

ReadlineChecker::ResultInputIsStdin ReadlineChecker::stdinInput(const Expr *input) {
  if (!this->processChecker->isProcessMember(input, "stdin")) {
    std::string message = ErrorTextReadline::notStdin();

    return err(ReadlineCheckerStdinInputError{
        .code = ReadlineCheckerStdinInputErrorCode::NOT_STDIN,
        .message = message,
    });
  }

  return ok(input);
}

Expr *ReadlineChecker::checkReadlineCall(CallExpr *call, NodeModule module, llvm::StringRef fn,
                                         llvm::SMLoc loc) {
  this->state.getInventoryStore().markUsesAsync();
  call->type = Type::error();
  auto loosely = [&] {
    for (Expr *&arg : call->args) {
      if (!this->processChecker->isProcessMember(arg, "stdin") &&
          !this->processChecker->isProcessMember(arg, "stdout") &&
          !this->processChecker->isProcessMember(arg, "stderr")) {
        arg = this->exprChecker->checkExpr(arg);
      }
    }

    return call;
  };
  ResultReadlineUsed function = ReadlineChecker::readlineFunction(fn, module);
  if (function.isErr()) {
    this->state.getDiagnostics().error(loc, function.error().message);
    return loosely();
  }
  bool rejected = this->callChecker->rejectSpread(call);
  ArgumentChecker::ResultArityMatches arity =
      rejected ? ok(call) : this->argumentChecker->checkArity(call, 1, 2);
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
    if (!this->fsChecker->nodeOptions(call, 0, fn,
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
        this->exprChecker->checkExpr(value);
      }
    }
    for (llvm::StringRef unsupported : {"prompt", "completer", "history"}) {
      if (Expr *value = options.lookup(unsupported)) {
        std::string message = ErrorTextReadline::option(unsupported);
        this->state.getDiagnostics().error(value->loc(), message);
        return call;
      }
    }
    if (!input) {
      std::string message = ErrorTextReadline::noInput();
      this->state.getDiagnostics().error(call->args[0]->loc(), message);
      return call;
    }
  } else {
    input = call->args[0];
    output = call->args.size() > 1 ? call->args[1] : nullptr;
  }
  ResultInputIsStdin stdinInput = this->stdinInput(input);
  if (stdinInput.isErr()) {
    this->state.getDiagnostics().error(input->loc(), stdinInput.error().message);
    return call;
  }
  int written = 0;
  if (output && !isa<UndefinedExpr>(output)) {
    if (this->processChecker->isProcessMember(output, "stdout")) {
      written = 1;
    } else if (this->processChecker->isProcessMember(output, "stderr")) {
      written = 2;
    } else {
      std::string message = ErrorTextReadline::output();
      this->state.getDiagnostics().error(output->loc(), message);
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
