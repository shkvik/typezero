#pragma once

#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/code/calls/call_rules/call_rules.h"
#include "compiler/semantic/code/calls/code_calls_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <optional>

namespace tz {

class NamedCalls {
public:
  NamedCalls(SemanticState &owner, CallRules &sharedCallRules);

  void checkNew(const CheckNew &task);
  void scopedCall(const ScopedCall &task);
  void afterNewLoad(const AfterNewLoad &task);
  void afterClassNew(const AfterClassNew &task);
  void checkDirectCall(const CheckDirectCall &task);
  void afterArrayLength(const AfterArrayLength &task);
  void afterArrayElement(const AfterArrayElement &task);
  void afterKnownFunctionType(const AfterKnownFunctionType &task);

private:
  using ResultHasZeroValue = Result<const Type *, CodeCallsRegistryZeroValueError>;
  using ResultArrayArgsFit = Result<const NewExpr *, CodeCallsRegistryArrayArgsError>;
  using ResultArrayElement = Result<const Type *, CodeCallsRegistryArrayElementError>;
  using ResultClassIsFound = Result<const NewExpr *, CodeCallsRegistryClassFoundError>;
  using ResultNoTypeArgsOk = Result<const NewExpr *, CodeCallsRegistryNoTypeArgsError>;
  using ResultCryptoMadeOk = Result<const ClassDecl *, CodeCallsRegistryCryptoNewError>;
  using ResultNamespaceNew = Result<const NewExpr *, CodeCallsRegistryNamespaceNewError>;
  using ResultSupportedNew = Result<const NewExpr *, CodeCallsRegistrySupportedNewError>;
  using ResultTypeArgsUsable = Result<const FuncSymbol *, CodeCallsRegistryDirectTypeArgsError>;

  void pushNewArgsLoosely(NewExpr *expr);
  void rejectNewTypeArgs(const NewExpr *expr);
  void startArrayNew(NewExpr *expr, const Type *expected);
  void finishArrayNew(NewExpr *expr, const Type *element);
  void startNotCallable(CallExpr *call, NameExpr *name, const ScopeEntry &entry);
  void startClassInstanceNew(NewExpr *expr, ClassDecl *classDecl, const Type *expected);
  void startNamespaceNew(NewExpr *expr, const Type *expected, std::optional<GlobalName> global);
  void startKnownFunctionCall(CallExpr *call, NameExpr *name, VarSymbol *variable,
                              const FunctionDecl *known);


  bool isCryptoClass(const ClassDecl *classDecl) const;
  bool startBuiltinNew(NewExpr *expr, const Type *expected, std::optional<GlobalName> global);


  ResultArrayArgsFit checkArrayArgs(const NewExpr *expr) const;
  ResultHasZeroValue checkZeroValue(const Type *element) const;
  ResultClassIsFound checkClassFound(const NewExpr *expr) const;
  ResultSupportedNew checkSupportedNew(const NewExpr *expr) const;
  ResultNamespaceNew checkNotNamespaced(const NewExpr *expr) const;
  ResultNoTypeArgsOk checkNoNewTypeArgs(const NewExpr *expr) const;
  ResultArrayElement checkArrayTypeArgs(const NewExpr *expr, bool isFromExpected) const;
  ResultCryptoMadeOk checkCryptoClassNew(const NewExpr *expr, const ClassDecl *classDecl) const;


  FunctionDecl *variableFunction(const VarSymbol *variable) const;
  ResultTypeArgsUsable checkDirectTypeArgs(const FuncSymbol *function,
                                           llvm::ArrayRef<TypeRef> typeArgs) const;

  CallRules &callRules;
  SemanticState &state;
};

}
