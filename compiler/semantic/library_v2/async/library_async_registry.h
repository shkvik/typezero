#pragma once

#include "compiler/program/library/async/timer_method_table/timer_method_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/library_v2/async/library_async_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <string>
#include <vector>

namespace tz {

class LibraryAsyncRegistry {
public:
  explicit LibraryAsyncRegistry(SemanticState &owner);

  void finishPromiseType();
  void checkAwait(const CheckAwait &task);
  void walkForAwait(const WalkForAwait &task);
  void checkNextTick(const CheckNextTick &task);
  void startAwaitBody(const StartAwaitBody &task);
  void afterAwaitValue(const AfterAwaitValue &task);
  void walkAsyncReturn(const WalkAsyncReturn &task);
  void checkPromiseNew(const CheckPromiseNew &task);
  void afterAnyNumber(const AfterAnyNumber &task);
  void checkTimerMethod(const CheckTimerMethod &task);
  void checkAsyncGlobal(const CheckAsyncGlobal &task);
  void afterAsyncSource(const AfterAsyncSource &task);
  void bindAwaitElement(const BindAwaitElement &task);
  void finishPromiseNew(const FinishPromiseNew &task);
  void finishPromiseThen(const FinishPromiseThen &task);
  void afterTaskCallback(const AfterTaskCallback &task);
  void afterResolveValue(const AfterResolveValue &task);
  void timerPromiseValue(const TimerPromiseValue &task);
  void checkPromiseMethod(const CheckPromiseMethod &task);
  void checkPromiseStatic(const CheckPromiseStatic &task);
  void resolvePromiseType(const ResolvePromiseType &task);
  void finishAsyncReturns(const FinishAsyncReturns &task);
  void afterAwaitIterable(const AfterAwaitIterable &task);
  void afterTimerCallback(const AfterTimerCallback &task);
  void afterTimerReceiver(const AfterTimerReceiver &task);
  void afterExecutorTrial(const AfterExecutorTrial &task);
  void enterAwaitLoopBody(const EnterAwaitLoopBody &task);
  void checkSchedulerCall(const CheckSchedulerCall &task);
  void nextPromiseElement(const NextPromiseElement &task);
  void afterPromiseValues(const AfterPromiseValues &task);
  void finishTimerPromise(const FinishTimerPromise &task);
  void timerPromiseOptions(const TimerPromiseOptions &task);
  void afterJoinedIterable(const AfterJoinedIterable &task);
  void afterPromiseTypeArg(const AfterPromiseTypeArg &task);
  void afterPromiseElement(const AfterPromiseElement &task);
  void finishWithResolvers(const FinishWithResolvers &task);
  void afterResolverTypeArg(const AfterResolverTypeArg &task);
  void afterAsyncUserIterator(const AfterAsyncUserIterator &task);
  void afterTimerPromiseValue(const AfterTimerPromiseValue &task);
  void checkTimersPromisesCall(const CheckTimersPromisesCall &task);

private:
  struct PromiseList {
    const Type *element;
    std::vector<const Type *> tuple;
  };

  using ResultTaskCallbackOk = Result<const Type *, LibraryAsyncRegistryTaskCallbackError>;
  using ResultAwaitedOneType = Result<const Type *, LibraryAsyncRegistryAwaitableError>;
  using ResultValueIsNotVoid = Result<const Type *, LibraryAsyncRegistryNotVoidError>;
  using ResultRaceTypeJoined = Result<const Type *, LibraryAsyncRegistryRaceJoinError>;
  using ResultTupleHasValues = Result<const Type *, LibraryAsyncRegistryAllValuesError>;
  using ResultEmptyListTyped = Result<const Type *, LibraryAsyncRegistryEmptyListError>;
  using ResultPlainValueType = Result<const Type *, LibraryAsyncRegistryPlainValueError>;
  using ResultListOfPromises = Result<const Type *, LibraryAsyncRegistryPromiseArrayError>;
  using ResultValueTypeKnown = Result<const Type *, LibraryAsyncRegistryResolverValueError>;
  using ResultBindingPattern = Result<const Type *, LibraryAsyncRegistryBindingShapeError>;
  using ResultPromiseMatches = Result<const Type *, LibraryAsyncRegistryPromiseMatchesError>;
  using ResultAwaitedElement = Result<const Type *, LibraryAsyncRegistryAwaitedElementError>;
  using ResultReturnsPromise = Result<const Type *, LibraryAsyncRegistryPromiseDeclaredError>;
  using ResultExecutorShaped = Result<const Expr *, LibraryAsyncRegistryExecutorShapeError>;
  using ResultInAsyncContext = Result<const FuncSymbol *, LibraryAsyncRegistryAsyncContextError>;
  using ResultTypeArgsAbsent = Result<const MemberExpr *, LibraryAsyncRegistryStaticTypeArgsError>;
  using ResultBindingUntyped = Result<const VarDeclarator *, LibraryAsyncRegistryBindingTypeError>;
  using ResultMethodAccepted = Result<void, LibraryAsyncRegistryCheckPromiseMethodError>;
  using ResultOneTypeArgOnly =
      Result<const MemberExpr *, LibraryAsyncRegistryResolverTypeArgsError>;

  void pushAnyNumber(Expr **slot);
  void pushLooseExecutor(Expr **slot);
  void inferExecutorValue(CallExpr *call);
  void reportExecutorValue(CallExpr *call);
  void walkSendsNothing(const Expr *iterable);
  void pushTypedExecutor(Expr **slot, const Type *value);
  void continuePromiseNew(CallExpr *call, const Type *value);
  void walkContext(ResultInAsyncContext context, llvm::SMLoc loc);
  void pushTaskCallback(Expr **slot, llvm::StringRef functionName);
  void tuplePromiseList(const CombinatorDraft &draft, const Expr *arg);
  void convertPromiseElements(ArrayLitExpr *literal, const Type *common);
  void emptyPromiseList(const CombinatorDraft &draft, ArrayLitExpr *literal);
  void typeCombinator(const CombinatorDraft &draft, const PromiseList &list);
  void finishPromiseLiteral(const CombinatorDraft &draft, ArrayLitExpr *literal);
  void checkClearCall(CallExpr *call, const NameExpr *name, bool isImmediate);
  void checkTimeoutCall(CallExpr *call, const NameExpr *name, bool isInterval);
  void settlePromiseThen(CallExpr *call, const Type *value, const Type *result);
  void checkTaskQueueCall(CallExpr *call, const NameExpr *name, bool isImmediate);
  void reportAfterArgs(CallExpr *call, llvm::SMLoc at, const std::string &message);
  void checkWithResolvers(CallExpr *call, MemberExpr *member, const Type *expected);
  void typeAllCall(CallExpr *call, const Type *element, const std::vector<const Type *> &tuple);
  void reportNoReject(CallExpr *call, const MemberExpr *member, const std::string &message);
  void readTimerOptions(const TimerPromiseDraft &draft, const Type *value, Expr **argument);
  void promisedTimer(CallExpr *call, const Type *value, bool isImmediate, bool isInterval) const;
  void finishAsyncReturn(const FuncSymbol *function, ReturnStmt *returnStmt, const Type *value);
  void startTimersPromises(CallExpr *call, llvm::StringRef functionName, llvm::SMLoc loc,
                           const Type *expected);
  void typeRaceCall(CallExpr *call, const PromiseList &list, const Type *wantedElement,
                    llvm::StringRef functionName);
  void checkResolve(CallExpr *call, const MemberExpr *member, const Type *wanted,
                    const std::string &functionName);
  void checkCombinator(CallExpr *call, const MemberExpr *member, const Type *wanted,
                       const std::string &functionName, Builtin builtin);


  bool isVoidValue(const Type *type) const;
  bool isTimerName(const NameExpr *name) const;
  bool isRearming(const Expr *callback) const;
  bool isOptionalAsyncIterable(const Type *type) const;
  bool isPlainFunction(const FuncSymbol *function) const;
  bool isSpreadOrHole(const ArrayLitExpr::Element &element) const;
  bool isResolvingWithValue(const Stmt *body, llvm::StringRef resolve) const;


  Expr *trueLiteral(llvm::SMLoc loc);
  Expr *delayArg(CallExpr *call, llvm::SMLoc loc);
  Expr *adaptedTask(Expr *arg, const Type *callback);


  const Type *awaited(const Type *type) const;
  const Type *wantedValue(const Type *expected) const;
  const Type *resolverType(const Type *value) const;
  const Type *withoutPromise(const Type *type) const;
  const Type *promisedHint(const Type *expected) const;
  const Type *loopElement(const Expr *iterable);
  const Type *expectedPromised(const Type *expected) const;
  const Type *expectedResolverValue(const Type *expected) const;
  const Type *unifiedType(llvm::ArrayRef<const Type *> types) const;
  const Type *combinatorElement(const Type *wanted, bool isRace) const;
  const Type *joinedValue(const Type *found, const Type *type) const;
  const Type *promiseHint(const Expr *value, const Type *expected) const;
  const Type *boundElement(const VarDeclarator &binding, const Type *element);
  const Type *resolvedValueType(const Stmt *body, llvm::StringRef resolve) const;
  const Type *settledElement(const Type *element, const Type *wantedElement) const;


  ResultInAsyncContext checkAwaitContext() const;
  ResultMethodAccepted checkNotResultMethod(const MemberExpr *member) const;
  ResultMethodAccepted checkIsThen(const CallExpr *call, const MemberExpr *member) const;
  ResultInAsyncContext checkForAwaitContext() const;
  ResultAwaitedOneType checkAwaitable(const Type *type) const;
  ResultValueTypeKnown checkResolverValue(const Type *value) const;
  ResultAwaitedElement checkAwaitedElement(const Type *type) const;
  ResultExecutorShaped checkExecutorShape(const Expr *executor) const;
  ResultTypeArgsAbsent checkStaticTypeArgs(const MemberExpr *member) const;
  ResultOneTypeArgOnly checkResolverTypeArgs(const MemberExpr *member) const;
  ResultTupleHasValues checkAllValues(llvm::ArrayRef<const Type *> tuple) const;
  ResultBindingUntyped checkBindingType(const VarDeclarator &binding) const;
  ResultValueIsNotVoid checkNotVoid(const Type *type, llvm::StringRef functionName) const;
  ResultPlainValueType checkPlainValue(const Type *type, llvm::StringRef functionName) const;
  ResultListOfPromises checkPromiseArray(const Expr *arg, llvm::StringRef functionName) const;
  ResultBindingPattern checkBindingShape(const VarDeclarator &binding, const Type *element) const;
  ResultEmptyListTyped checkEmptyList(const Type *wantedElement,
                                      llvm::StringRef functionName) const;
  ResultRaceTypeJoined checkRaceElement(const PromiseList &list,
                                        llvm::StringRef functionName) const;
  ResultReturnsPromise checkPromiseDeclared(const FunctionDecl *functionDecl,
                                            const Type *declared) const;
  ResultPromiseMatches checkPromiseMatches(const Type *given, const Type *value,
                                           const FuncSymbol *function) const;
  ResultRaceTypeJoined checkRaceJoin(const Type *element, const Type *type,
                                     llvm::StringRef functionName) const;
  ResultTaskCallbackOk checkTaskCallbackType(const Type *type, const Type *task,
                                             const Expr *adapted,
                                             llvm::StringRef functionName) const;


  Builtin timerMethodBuiltin(TimerMethod method) const;
  FunctionDecl *namedFunction(const Expr *expr) const;
  std::string asyncHint(const FunctionDecl *functionDecl) const;
  const Expr *soleStatementValue(const Stmt *statement) const;
  const CallExpr *resolveCall(const Expr *expr, llvm::StringRef resolve) const;
  std::vector<const CallExpr *> resolveCalls(const Stmt *body, llvm::StringRef resolve) const;
  Signature thenSignature(const Expr *callback, const MemberExpr *member,
                          const Type *expected) const;

  SemanticState &state;
};

}
