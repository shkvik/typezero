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
  ResultReadlineUsed function = ReadlineChecker::readlineFunction(fn, module);
  if (function.isErr()) {
    this->state.getDiagnostics().error(loc, function.error().message);
    return this->checkArgsLoosely(call);
  }
  if (this->hasRejectedArgs(call, loc)) {
    return call;
  }
  std::optional<ReadlineStreams> streams = this->readlineStreams(call, fn);
  if (!streams.has_value()) {
    return call;
  }
  ResultInputIsStdin stdinInput = this->stdinInput(streams->input);
  if (stdinInput.isErr()) {
    this->state.getDiagnostics().error(streams->input->loc(), stdinInput.error().message);
    return call;
  }
  std::optional<int> written = this->outputStream(streams->output);
  if (!written.has_value()) {
    return call;
  }
  auto *where = this->state.getAst().make<IntLitExpr>(loc, uint64_t(*written));
  where->type = Type::intTy();
  call->args = {where};
  call->target = BuiltinCall{.builtin = Builtin::RlCreate};
  Type::Runtime runtime =
      module == NodeModule::Readline ? Type::Runtime::Readline : Type::Runtime::ReadlinePromises;
  call->type = Type::runtimeObject(runtime);

  return call;
}

Expr *ReadlineChecker::checkArgsLoosely(CallExpr *call) {
  for (Expr *&arg : call->args) {
    bool stream = this->processChecker->isProcessMember(arg, "stdin") ||
                  this->processChecker->isProcessMember(arg, "stdout") ||
                  this->processChecker->isProcessMember(arg, "stderr");
    if (!stream) {
      arg = this->exprChecker->checkExpr(arg);
    }
  }

  return call;
}

bool ReadlineChecker::hasRejectedArgs(CallExpr *call, llvm::SMLoc loc) {
  bool rejected = this->callChecker->rejectSpread(call);
  ArgumentChecker::ResultArityMatches arity =
      rejected ? ok(call) : this->argumentChecker->checkArity(call, 1, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
  }

  return rejected || arity.isErr();
}

std::optional<ReadlineChecker::ReadlineStreams>
ReadlineChecker::readlineStreams(CallExpr *call, llvm::StringRef fn) {
  if (isa<ObjectLitExpr>(call->args[0])) {
    return this->optionStreams(call, fn);
  }

  return ReadlineStreams{
      .input = call->args[0],
      .output = call->args.size() > 1 ? call->args[1] : nullptr,
  };
}

std::optional<ReadlineChecker::ReadlineStreams> ReadlineChecker::optionStreams(CallExpr *call,
                                                                               llvm::StringRef fn) {
  llvm::StringMap<Expr *> options;
  bool read = this->fsChecker->nodeOptions(call, 0, fn,
                                           {"input", "output", "terminal", "crlfDelay",
                                            "historySize", "removeHistoryDuplicates", "prompt",
                                            "tabSize", "completer", "escapeCodeTimeout", "history"},
                                           options);
  if (!read) {
    return std::nullopt;
  }
  ReadlineStreams streams{
      .input = options.lookup("input"),
      .output = options.lookup("output"),
  };
  for (llvm::StringRef ignored : {"terminal", "crlfDelay", "historySize", "removeHistoryDuplicates",
                                  "tabSize", "escapeCodeTimeout"}) {
    Expr *value = options.lookup(ignored);
    if (value != nullptr) {
      this->exprChecker->checkExpr(value);
    }
  }
  for (llvm::StringRef unsupported : {"prompt", "completer", "history"}) {
    Expr *value = options.lookup(unsupported);
    if (value != nullptr) {
      std::string message = ErrorTextReadline::option(unsupported);
      this->state.getDiagnostics().error(value->loc(), message);
      return std::nullopt;
    }
  }
  if (streams.input == nullptr) {
    std::string message = ErrorTextReadline::noInput();
    this->state.getDiagnostics().error(call->args[0]->loc(), message);
    return std::nullopt;
  }

  return streams;
}

std::optional<int> ReadlineChecker::outputStream(const Expr *output) {
  if (output == nullptr || isa<UndefinedExpr>(output)) {
    return 0;
  }
  if (this->processChecker->isProcessMember(output, "stdout")) {
    return 1;
  }
  if (this->processChecker->isProcessMember(output, "stderr")) {
    return 2;
  }
  std::string message = ErrorTextReadline::output();
  this->state.getDiagnostics().error(output->loc(), message);

  return std::nullopt;
}
}
