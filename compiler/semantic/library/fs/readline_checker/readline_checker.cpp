#include "compiler/semantic/library/fs/readline_checker/readline_checker.h"
#include "compiler/program/library/fs/fs_export_table/fs_export_table.h"
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
  this->fsChecker = &all.fsChecker;
  this->callChecker = &all.callChecker;
  this->exprChecker = &all.exprChecker;
  this->processChecker = &all.processChecker;
  this->argumentChecker = &all.argumentChecker;
}

ReadlineChecker::ResultReadlineUsed
ReadlineChecker::checkReadlineFunction(llvm::StringRef functionName, NodeModule module) const {
  if (FsExportTable::find(module, functionName) != FsExport::FUNCTION) {
    auto message = ErrorTextReadline::typeOnly(functionName, module != NodeModule::Readline);

    return err(ReadlineCheckerReadlineFunctionError{
        .code = ReadlineCheckerReadlineFunctionErrorCode::TYPE_ONLY,
        .message = message,
    });
  }

  return ok(functionName);
}

ReadlineChecker::ResultInputIsStdin ReadlineChecker::checkStdinInput(const Expr *input) {
  if (!this->processChecker->isProcessMember(input, ProcessMember::STDIN)) {
    auto message = ErrorTextReadline::notStdin();

    return err(ReadlineCheckerStdinInputError{
        .code = ReadlineCheckerStdinInputErrorCode::NOT_STDIN,
        .message = message,
    });
  }

  return ok(input);
}

Expr *ReadlineChecker::checkReadlineCall(CallExpr *call, NodeModule module,
                                         llvm::StringRef functionName, llvm::SMLoc loc) {
  this->state.getInventoryStore().markUsesAsync();
  call->type = Type::error();
  auto function = this->checkReadlineFunction(functionName, module);
  if (function.isErr()) {
    this->state.getDiagnostics().error(loc, function.error().message);
    return this->checkArgsLoosely(call);
  }
  if (this->hasRejectedArgs(call, loc)) {
    return call;
  }
  std::optional<ReadlineStreams> streams = this->readlineStreams(call, functionName);
  if (!streams.has_value()) {
    return call;
  }
  auto stdinInput = this->checkStdinInput(streams->input);
  if (stdinInput.isErr()) {
    this->state.getDiagnostics().error(streams->input->loc(), stdinInput.error().message);
    return call;
  }
  auto written = this->outputStream(streams->output);
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
    bool isStream = this->processChecker->isProcessMember(arg, ProcessMember::STDIN) ||
                    this->processChecker->isProcessMember(arg, ProcessMember::STDOUT) ||
                    this->processChecker->isProcessMember(arg, ProcessMember::STDERR);
    if (!isStream) {
      arg = this->exprChecker->checkExpr(arg);
    }
  }

  return call;
}

bool ReadlineChecker::hasRejectedArgs(CallExpr *call, llvm::SMLoc loc) {
  bool isRejected = this->callChecker->rejectSpread(call);
  auto arity = isRejected ? ok(call) : this->argumentChecker->checkArity(call, 1, 2);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(loc, arity.error().message);
  }

  return isRejected || arity.isErr();
}

std::optional<ReadlineChecker::ReadlineStreams>
ReadlineChecker::readlineStreams(CallExpr *call, llvm::StringRef functionName) {
  if (isa<ObjectLitExpr>(call->args[0])) {
    return this->optionStreams(call, functionName);
  }

  return ReadlineStreams{
      .input = call->args[0],
      .output = call->args.size() > 1 ? call->args[1] : nullptr,
  };
}

std::optional<ReadlineChecker::ReadlineStreams>
ReadlineChecker::optionStreams(CallExpr *call, llvm::StringRef functionName) {
  auto options = NodeOptions();
  bool isRead = this->fsChecker->readNodeOptions(
      call, 0, functionName,
      {NodeOption::INPUT, NodeOption::OUTPUT, NodeOption::TERMINAL, NodeOption::CRLF_DELAY,
       NodeOption::HISTORY_SIZE, NodeOption::REMOVE_HISTORY_DUPLICATES, NodeOption::PROMPT,
       NodeOption::TAB_SIZE, NodeOption::COMPLETER, NodeOption::ESCAPE_CODE_TIMEOUT,
       NodeOption::HISTORY},
      options);
  if (!isRead) {
    return std::nullopt;
  }
  ReadlineStreams streams{
      .input = options.lookup(NodeOption::INPUT),
      .output = options.lookup(NodeOption::OUTPUT),
  };
  for (NodeOption ignored : {NodeOption::TERMINAL, NodeOption::CRLF_DELAY, NodeOption::HISTORY_SIZE,
                             NodeOption::REMOVE_HISTORY_DUPLICATES, NodeOption::TAB_SIZE,
                             NodeOption::ESCAPE_CODE_TIMEOUT}) {
    Expr *value = options.lookup(ignored);
    if (value != nullptr) {
      this->exprChecker->checkExpr(value);
    }
  }
  for (NodeOption unsupported : {NodeOption::PROMPT, NodeOption::COMPLETER, NodeOption::HISTORY}) {
    Expr *value = options.lookup(unsupported);
    if (value != nullptr) {
      auto name = NodeOptionTable::getName(unsupported);
      auto message = ErrorTextReadline::option(name);
      this->state.getDiagnostics().error(value->loc(), message);
      return std::nullopt;
    }
  }
  if (streams.input == nullptr) {
    auto message = ErrorTextReadline::noInput();
    this->state.getDiagnostics().error(call->args[0]->loc(), message);
    return std::nullopt;
  }

  return streams;
}

std::optional<int> ReadlineChecker::outputStream(const Expr *output) {
  if (output == nullptr || isa<UndefinedExpr>(output)) {
    return 0;
  }
  if (this->processChecker->isProcessMember(output, ProcessMember::STDOUT)) {
    return 1;
  }
  if (this->processChecker->isProcessMember(output, ProcessMember::STDERR)) {
    return 2;
  }
  auto message = ErrorTextReadline::output();
  this->state.getDiagnostics().error(output->loc(), message);

  return std::nullopt;
}

}
