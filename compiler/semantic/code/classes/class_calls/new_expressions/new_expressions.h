#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/classes/class_calls/class_call_rules/class_call_rules.h"
#include "compiler/semantic/code/classes/class_facts/class_facts.h"
#include "compiler/semantic/code/classes/code_classes_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class NewExpressions {
public:
  NewExpressions(SemanticState &owner, ClassFacts &sharedFacts,
                 ClassCallRules &sharedClassCallRules);

  void checkValueNew(const CheckValueNew &task);
  void checkClassNew(const CheckClassNew &task);
  void finishClassNew(const FinishClassNew &task);
  void checkConstructorAccess(const CheckConstructorAccess &task);
  void nextReflectedArg(const NextReflectedArg &task);
  void afterReflectedArg(const AfterReflectedArg &task);
  void afterValueNewCallee(const AfterValueNewCallee &task);
  void afterConstructorChoice(const AfterConstructorChoice &task);

private:
  using ResultClassHasItsType = Result<ClassDecl *, CodeClassesRegistryTypedClassError>;
  using ResultArgumentNotVoid = Result<const Expr *, CodeClassesRegistryArgumentValueError>;
  using ResultNewOfClassValue = Result<const Type *, CodeClassesRegistryValueNewClassError>;
  using ResultClassNotRuntime = Result<const ClassDecl *, CodeClassesRegistryNotRuntimeError>;
  using ResultClassIsConcrete = Result<const ClassDecl *, CodeClassesRegistryClassConcreteError>;
  using ResultClassIsDeclared = Result<const ClassDecl *, CodeClassesRegistryClassDeclaredError>;
  using ResultNoTypeArguments = Result<const NewExpr *, CodeClassesRegistryRuntimeTypeArgsError>;
  using ResultArgumentsAbsent = Result<const CallExpr *, CodeClassesRegistryArgumentsAbsentError>;
  using ResultMethodCallNoTypeArguments =
      Result<const NewExpr *, CodeClassesRegistryClassTypeArgsError>;

  void failNew(NewExpr *expr);
  void finishReflectNew(const ReflectNewDraft &draft);
  void walkNewClass(const NewExpr *expr, const ClassDecl *classDecl);
  void startClassValueNew(NewExpr *expr, ClassDecl *classDecl, const Type *expected);


  bool hasInitializer(const ClassDecl *classDecl) const;


  ResultArgumentNotVoid checkArgumentValue(const Expr *arg) const;
  ResultNewOfClassValue checkValueNewClass(const Type *type) const;
  ResultClassHasItsType checkTypedClass(ClassDecl *classDecl) const;
  ResultNoTypeArguments checkRuntimeTypeArgs(const NewExpr *expr) const;
  ResultClassNotRuntime checkNotRuntime(const ClassDecl *classDecl) const;
  ResultClassIsConcrete checkClassConcrete(const ClassDecl *classDecl) const;
  ResultClassIsDeclared checkStaticOwnerDeclared(const ClassDecl *classDecl) const;
  ResultArgumentsAbsent checkArgumentsAbsent(const CallExpr *call,
                                             const ClassDecl *classDecl) const;


  ResultMethodCallNoTypeArguments checkClassTypeArgs(const NewExpr *expr,
                                                     const ClassDecl *classDecl) const;

  ClassFacts &facts;
  SemanticState &state;
  ClassCallRules &classCallRules;
};

}
