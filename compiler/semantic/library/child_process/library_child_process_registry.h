#pragma once

#include "compiler/program/library/builtin_modules/node_option_table/node_option_table.h"
#include "compiler/program/library/child_process/child_function_table/child_function_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/library/child_process/library_child_process_registry_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class LibraryChildProcessRegistry {
public:
  explicit LibraryChildProcessRegistry(SemanticState &owner);

  void afterChildArgs(const AfterChildArgs &task);
  void afterChildFile(const AfterChildFile &task);
  void checkChildCall(const CheckChildCall &task);
  void walkChildStdio(const WalkChildStdio &task);
  void nextStdioEntry(const NextStdioEntry &task);
  void afterChildShell(const AfterChildShell &task);
  void afterChildInput(const AfterChildInput &task);
  void checkChildInput(const CheckChildInput &task);
  void finishChildCall(const FinishChildCall &task);
  void walkChildLimits(const WalkChildLimits &task);
  void nextChildOption(const NextChildOption &task);
  void finishChildShell(const FinishChildShell &task);
  void finishChildInput(const FinishChildInput &task);
  void afterChildOption(const AfterChildOption &task);
  void afterChildOptions(const AfterChildOptions &task);
  void afterChildKillSignal(const AfterChildKillSignal &task);
  void finishChildKillSignal(const FinishChildKillSignal &task);

private:
  using ResultStdioKind = Result<unsigned, LibraryChildProcessRegistryStdioKindError>;
  using ResultFileName = Result<const Expr *, LibraryChildProcessRegistryFileNameError>;

  void typeStdioLiteral(Expr *given);
  void finishChildRun(ChildRunDraft &run);
  void walkShellOption(ChildRunDraft &run);
  void walkInputOption(ChildRunDraft &run);
  void readChildOptions(ChildRunDraft &run);
  void walkStdioOption(ChildRunDraft &run);
  void walkOptionLimits(ChildRunDraft &run);
  void walkEncodingOption(ChildRunDraft &run);
  void walkKillSignalOption(ChildRunDraft &run);
  void pushDroppedCheck(Expr *value, CheckTask next);
  void pushCoercedValue(Expr *value, const Type *type);
  void pushAbsentValue(llvm::SMLoc loc, const Type *type);
  void walkExecShell(ChildRunDraft &run, Expr *shell);
  void walkStdioText(ChildRunDraft &run, Expr *value);
  void walkZeroArguments(ChildRunDraft &run, const Expr *given);


  bool hasZero(const Expr *expr) const;
  bool isNegativeLiteral(const Expr *value) const;


  ResultFileName checkFileName(const Expr *file, bool isExec) const;
  ResultStdioKind checkStdioKind(const Expr *given, unsigned index) const;
  ResultStdioKind checkStdioEntry(const ArrayLitExpr::Element &element, unsigned index);


  const Type *plainOptionType(NodeOption option) const;
  IntLitExpr *makeIntLiteral(llvm::SMLoc loc, uint64_t value);
  Expr *optionOf(const ChildRunDraft &run, NodeOption option) const;

  SemanticState &state;
};

}
