#pragma once

#include "compiler/semantic/semantic_state/semantic_state.h"
#include <vector>

namespace tz {

class LibraryCommonRegistry {
public:
  explicit LibraryCommonRegistry(SemanticState &owner);

  void markCallFailed(const MarkCallFailed &task);
  void pushCheckedValue(const PushCheckedValue &task);
  void preludeTypeOf(const PreludeTypeOf &task);
  void answerAliasType(const AnswerAliasType &task);
  void callLibraryStatic(const CallLibraryStatic &task);
  void callLibraryMethod(const CallLibraryMethod &task);
  void afterMethodReturn(const AfterMethodReturn &task);
  void afterMethodCallee(const AfterMethodCallee &task);
  void afterLibraryMemberType(const AfterLibraryMemberType &task);
  void finishLibraryCall(const FinishLibraryCall &task);
  void afterLibraryCallee(const AfterLibraryCallee &task);
  void afterLibraryReturn(const AfterLibraryReturn &task);
  void callLibraryFunction(const CallLibraryFunction &task);
  void readNodeOptions(const ReadNodeOptions &task);
  void rejectSpreadCall(const RejectSpreadCall &task);

private:
  bool isMentioningParam(const Type *type) const;
  void pushLooseCheck(Expr **slot);
  void targetMethod(const CallLibraryMethod &draft, FuncSymbol *function);
  std::vector<llvm::StringRef> optionNames(llvm::ArrayRef<NodeOption> options) const;
  void startLibraryCall(CallExpr *call, FuncSymbol *function,
                        const std::vector<const Type *> &bound, const std::vector<bool> &given,
                        llvm::SMLoc loc);

  SemanticState &state;
};

}
