#pragma once

#include "compiler/program/library/result/result_static_table/result_static_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/library_v2/result/library_result_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <string>
#include <vector>

namespace tz {

class LibraryResultRegistry {
public:
  explicit LibraryResultRegistry(SemanticState &owner);

  void finishOkErr(const FinishOkErr &task);
  void neverOperands(const NeverOperands &task);
  void checkUsedValue(const CheckUsedValue &task);
  void resultHalfType(const ResultHalfType &task);
  void afterHalfValue(const AfterHalfValue &task);
  void checkResultCall(const CheckResultCall &task);
  void afterUnwrapOrArg(const AfterUnwrapOrArg &task);
  void afterResultMatch(const AfterResultMatch &task);
  void finishResultType(const FinishResultType &task);
  void afterCombineList(const AfterCombineList &task);
  void afterHalfCallback(const AfterHalfCallback &task);
  void checkResultStatic(const CheckResultStatic &task);
  void checkResultMember(const CheckResultMember &task);
  void checkResultMethod(const CheckResultMethod &task);
  void resolveResultType(const ResolveResultType &task);
  void afterCombineArray(const AfterCombineArray &task);
  void afterWrittenResult(const AfterWrittenResult &task);
  void resultHalfCallback(const ResultHalfCallback &task);
  void afterResultCallback(const AfterResultCallback &task);
  void afterPlainHalfCallback(const AfterPlainHalfCallback &task);
  void checkUsedCallbackValue(const CheckUsedCallbackValue &task);

private:
  enum class Discarded {
    BY_STATEMENT,
    BY_CALLBACK,
  };

  using ResultStaticIsKnown = Result<ResultStatic, LibraryResultRegistryStaticMethodError>;
  using ResultValueGetsUsed = Result<const Expr *, LibraryResultRegistryUsedValueError>;
  using ResultOkErrTypeArgs = Result<const NameExpr *, LibraryResultRegistryOkErrTypeArgsError>;
  using ResultTypeArgsPair = Result<const TypeRef *, LibraryResultRegistryResultTypeArgsError>;

  void checkHalfMember(MemberExpr *member, ResultHalf half);
  void typeResultCall(const ResultMethodDraft &run, const Type *returnType);
  void typeOrElseCall(const ResultMethodDraft &run, const Type *returnType);
  void typeChainedCall(const ResultMethodDraft &run, const Type *returnType);
  void checkResultMatch(const ResultMethodDraft &run, const Type *expected);
  void pushCallbackReturn(const ResultMethodDraft &run, const Type *expected);
  void startHalfValue(CallExpr *call, NameExpr *name, ResultHalf half, const Type *wanted);
  void checkOkErr(CallExpr *call, NameExpr *name, ResultHalf half, const Type *expected);
  void reportNoValues(const ResultCombineDraft &combine, const Type *result, const Expr *at);
  void checkSameCombine(const ResultCombineDraft &combine, ArrayLitExpr *literal,
                        const Type *common);
  void checkTupleCombine(const ResultCombineDraft &combine, ArrayLitExpr *literal,
                         const std::vector<const Type *> &results);


  bool isGivingType(const Type *half) const;
  bool isPlainList(const ArrayLitExpr *literal) const;
  bool reportNotResult(const ResultMethodDraft &run, const Type *returnType);
  bool isHoldingValues(const ResultCombineDraft &combine, const Type *result) const;


  const Type *halfParam(const Type *half) const;
  const Type *wantedType(const Type *expected) const;
  const Type *expectedResult(const Type *expected) const;
  const Type *halfType(const Type *result, ResultHalf half) const;
  const Type *otherHalfType(const Type *result, ResultHalf half) const;
  const Type *sameValues(const Type *first, const Type *second) const;
  const Type *joinedValue(const Type *first, const Type *second) const;
  const Type *commonResult(llvm::ArrayRef<const Type *> results) const;
  const Type *errorsOf(const ResultCombineDraft &combine, const Type *error) const;
  const Type *callbackReturnType(Builtin builtin, const Type *wantedResult, bool isOnError) const;


  ResultValueGetsUsed checkUsed(const Expr *expr, Discarded how, llvm::SMLoc at);
  ResultValueGetsUsed checkPromiseUsed(const Expr *expr, Discarded how, llvm::SMLoc at);


  std::string receiverText(const Expr *expr) const;
  ResultTypeArgsPair checkResultTypeArgs(const TypeRef &ref) const;
  ResultOkErrTypeArgs checkOkErrTypeArgs(const NameExpr *name) const;
  ResultMethodDraft resultCallOf(CallExpr *call, MemberExpr *member) const;
  ResultStaticIsKnown checkStaticMethod(const NameExpr *name, llvm::StringRef method) const;

  SemanticState &state;
};

}
