#pragma once

#include "compiler/program/library/child_process/child_function_table/child_function_table.h"
#include "compiler/semantic/library/child_process/child_checker/child_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "llvm/ADT/StringMap.h"
#include <array>
#include <optional>
#include <vector>

namespace tz {

class SemanticParts;
class FsChecker;
class CallChecker;
class ExprChecker;
class BytesChecker;
class TypeRelations;
class ArgumentChecker;
class ChildErrorTypes;

class ChildChecker {
public:
  ChildChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkChildCall(CallExpr *call, llvm::StringRef functionName, llvm::SMLoc loc);

private:
  const Type *bufferType() const;
  bool isKnownSignal(llvm::StringRef name) const;
  bool hasZero(const Expr *expr) const;

  struct ChildRun {
    CallExpr *call = nullptr;
    ChildFunction function = ChildFunction::SPAWN_SYNC;
    llvm::StringRef functionName;
    llvm::SMLoc loc;
    bool isExec = false;
    bool isFailed = false;
    std::vector<Expr *> args;
    llvm::StringMap<Expr *> options;
    unsigned encoding = 0;
    std::array<unsigned, 3> stdio{};
    bool isStdioGiven = false;
    bool isViaShell = false;
  };

  void walkFileArgument(ChildRun &run);
  void fillPlainOptions(ChildRun &run);
  void walkInputOption(ChildRun &run);
  void walkShellOption(ChildRun &run);
  void walkStdioOption(ChildRun &run);
  void walkOptionLimits(ChildRun &run);
  Expr *finishChildCall(ChildRun &run);
  void walkEncodingOption(ChildRun &run);
  void walkKillSignalOption(ChildRun &run);
  void walkExecShell(ChildRun &run, Expr *shell);
  void walkStdioText(ChildRun &run, Expr *value);
  std::optional<size_t> walkArgumentList(ChildRun &run);
  Expr *absentValue(llvm::SMLoc loc, const Type *type);
  std::optional<unsigned> stdioKind(Expr *given, unsigned index);
  IntLitExpr *makeIntLiteral(llvm::SMLoc loc, uint64_t value);
  void walkZeroArguments(ChildRun &run, const Expr *given);
  void walkStdioList(ChildRun &run, const ArrayLitExpr *list);
  Expr *optionValue(ChildRun &run, llvm::StringRef name, const Type *type);

  SemanticState &state;
  FsChecker *fsChecker = nullptr;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  BytesChecker *bytesChecker = nullptr;
  TypeRelations *typeRelations = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  ChildErrorTypes *childErrorTypes = nullptr;
};

}
