#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/iterators/code_iterators_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class IterableSources {
public:
  explicit IterableSources(SemanticState &owner);

  void iteratorOf(const IteratorOf &task);
  void findIterator(const FindIterator &task);
  void userIterator(const UserIterator &task);
  void iterableOfSlot(const IterableOfSlot &task);
  void buildIterateCall(const BuildIterateCall &task);
  void afterIterableUser(const AfterIterableUser &task);
  void asyncUserIterator(const AsyncUserIterator &task);
  void finishIterateCall(const FinishIterateCall &task);
  void afterIteratorValue(const AfterIteratorValue &task);
  void afterSymbolIterator(const AfterSymbolIterator &task);
  void afterResultIterator(const AfterResultIterator &task);
  void finishIterateSymbol(const FinishIterateSymbol &task);
  void afterBuiltinIterator(const AfterBuiltinIterator &task);
  void destructuredIteratorSlot(const DestructuredIteratorSlot &task);
  void afterAsyncSymbolIterator(const AfterAsyncSymbolIterator &task);

private:
  using ResultIteratorGiven = Result<const Expr *, CodeIteratorsRegistryIteratorGivenError>;
  using ResultAsyncIterator = Result<const Expr *, CodeIteratorsRegistryAsyncIteratorGivenError>;

  void startIterateSymbol();
  void pushIterateCall(Expr *it, const Type *value, FuncSymbol *iterate);


  Expr *collectionValues(Expr *expr);
  Expr *takeCall(Expr *init, const VarDeclarator &declarator);
  Expr *dictCall(llvm::SMLoc loc, Expr *receiver, llvm::StringRef name, Builtin builtin,
                 const Type *type);


  ResultIteratorGiven checkIteratorGiven(const Type *type, const Expr *result) const;
  ResultAsyncIterator checkAsyncIteratorGiven(const Type *type, const Expr *result) const;


  ClassDecl *classOf(const Type *type) const;
  CallExpr *symbolMethodCall(Expr *expr, llvm::StringRef name);

  SemanticState &state;
};

}
