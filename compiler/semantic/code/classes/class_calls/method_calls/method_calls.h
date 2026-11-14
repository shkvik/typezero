#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/classes/class_calls/class_call_rules/class_call_rules.h"
#include "compiler/semantic/code/classes/class_facts/class_facts.h"
#include "compiler/semantic/code/classes/code_classes_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <vector>

namespace tz {

class MethodCalls {
public:
  MethodCalls(SemanticState &owner, ClassFacts &sharedFacts, ClassCallRules &sharedClassCallRules);

  void checkMethodCall(const CheckMethodCall &task);
  void afterBoundTypes(const AfterBoundTypes &task);
  void afterMethodChoice(const AfterMethodChoice &task);
  void checkOverloadCall(const CheckOverloadCall &task);
  void afterMethodCallTry(const AfterMethodCallTry &task);
  void afterGenericCallee(const AfterGenericCallee &task);
  void afterOverloadChoice(const AfterOverloadChoice &task);
  void afterPlainMethodType(const AfterPlainMethodType &task);
  void checkClassStaticCall(const CheckClassStaticCall &task);
  void afterClassFieldCallee(const AfterClassFieldCallee &task);
  void afterGenericMethodCall(const AfterGenericMethodCall &task);
  void afterEventsFunctionTry(const AfterEventsFunctionTry &task);
  void afterStaticFieldCallee(const AfterStaticFieldCallee &task);
  void afterStaticMethodChoice(const AfterStaticMethodChoice &task);

private:
  using ResultFieldIsCallable = Result<const Type *, CodeClassesRegistryFieldCallableError>;
  using ResultFieldsAreAllSet = Result<const MemberExpr *, CodeClassesRegistryFieldsSetError>;
  using ResultStaticFieldIsCallable = Result<const Type *, CodeClassesRegistryStaticCallableError>;
  using ResultTypeArgsAllowed = Result<const FuncSymbol *, CodeClassesRegistryMethodTypeArgsError>;
  using ResultMethodsAreFound =
      Result<std::vector<FunctionDecl *>, CodeClassesRegistryMethodCallInstanceMethodsError>;

  void nextMethodCall(const MethodCallDraft &draft);
  void walkMethodAccess(MemberExpr *member, FunctionDecl *method);
  void setGenericTarget(CallExpr *call, FuncSymbol *callee, GenericCallee kind) const;
  void startClassMethodCall(CallExpr *call, MemberExpr *member, ClassDecl *classDecl,
                            const Type *expected);


  bool isInstantiatedNow(const FuncSymbol *function) const;
  bool isEventTargetClass(const ClassDecl *classDecl) const;


  ResultFieldsAreAllSet checkFieldsSetBeforeCall(const MemberExpr *member) const;
  ResultFieldIsCallable checkFieldCallable(const Type *type, llvm::StringRef name) const;
  ResultMethodsAreFound checkCalledMethods(const ClassDecl *classDecl, llvm::StringRef name) const;
  ResultTypeArgsAllowed checkMethodTypeArgs(const FuncSymbol *function,
                                            const MemberExpr *member) const;
  ResultTypeArgsAllowed checkStaticMethodTypeArgs(const FuncSymbol *function,
                                                  const MemberExpr *member) const;


  MethodCall methodTarget(ClassDecl *classDecl, FuncSymbol *function) const;
  ResultStaticFieldIsCallable checkStaticCallable(const Type *type, const ClassDecl *classDecl,
                                                  llvm::StringRef name) const;

  ClassFacts &facts;
  SemanticState &state;
  ClassCallRules &classCallRules;
};

}
