#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/classes/class_calls/class_call_rules/class_call_rules.h"
#include "compiler/semantic/code/classes/class_facts/class_facts.h"
#include "compiler/semantic/code/classes/code_classes_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <vector>

namespace tz {

class SuperCalls {
public:
  SuperCalls(SemanticState &owner, ClassFacts &sharedFacts, ClassCallRules &sharedClassCallRules);

  void checkSuperCall(const CheckSuperCall &task);
  void finishSuperCall(const FinishSuperCall &task);
  void afterSuperObject(const AfterSuperObject &task);
  void checkSuperMethodCall(const CheckSuperMethodCall &task);
  void afterSuperEmitterTry(const AfterSuperEmitterTry &task);
  void afterSuperMethodChoice(const AfterSuperMethodChoice &task);
  void afterParentConstructorChoice(const AfterParentConstructorChoice &task);

private:
  using ResultMethodHasItsBody = Result<FuncSymbol *, CodeClassesRegistryMethodBodyError>;
  using ResultParentClassFound = Result<ClassDecl *, CodeClassesRegistryParentClassError>;
  using ResultSuperIsStatement = Result<const CallExpr *, CodeClassesRegistryRootSuperError>;
  using ResultConstructorScope = Result<FunctionDecl *, CodeClassesRegistrySuperAllowedError>;
  using ResultSuperMethodScope = Result<FunctionDecl *, CodeClassesRegistrySuperContextError>;
  using ResultSuperNotRepeated = Result<const ClassDecl *, CodeClassesRegistrySuperOnceError>;
  using ResultNoSuperArguments = Result<const CallExpr *, CodeClassesRegistrySuperArgumentsError>;
  using ResultObjectIsComplete = Result<const MemberExpr *, CodeClassesRegistryObjectCompleteError>;
  using ResultParentHasMethods =
      Result<std::vector<FunctionDecl *>, CodeClassesRegistryParentMethodsError>;

  void startSuperMethodChoice(CallExpr *call, MemberExpr *member,
                              const std::vector<FunctionDecl *> &methods);


  FunctionDecl *enclosingMethod();
  FunctionDecl *methodWithThis(FunctionDecl *functionDecl) const;


  ResultSuperIsStatement checkRootSuper(const CallExpr *call) const;
  ResultParentClassFound checkParentClass(ClassDecl *classDecl) const;
  ResultSuperMethodScope checkSuperContext(FunctionDecl *method) const;
  ResultSuperNotRepeated checkSuperOnce(const ClassDecl *classDecl) const;
  ResultConstructorScope checkSuperAllowed(FunctionDecl *constructor) const;
  ResultObjectIsComplete checkObjectComplete(const MemberExpr *member) const;
  ResultMethodHasItsBody checkMethodBody(FuncSymbol *function, llvm::StringRef name) const;
  ResultParentHasMethods checkParentMethods(const ClassDecl *parent, llvm::StringRef name) const;
  ResultNoSuperArguments checkSuperArguments(const CallExpr *call, const ClassDecl *parent) const;


  ClassDecl *superCallClass(const CallExpr *call);

  ClassFacts &facts;
  SemanticState &state;
  ClassCallRules &classCallRules;
};

}
