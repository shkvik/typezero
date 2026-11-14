#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/classes/class_facts/class_facts.h"
#include "compiler/semantic/code/classes/code_classes_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <vector>

namespace tz {

class ClassStatements {
public:
  ClassStatements(SemanticState &owner, ClassFacts &sharedFacts);

  void leaveStaticFields();
  void leaveDeclaringClass();
  void afterLateBase(const AfterLateBase &task);
  void checkClassStmt(const CheckClassStmt &task);
  void checkClassExpr(const CheckClassExpr &task);
  void finishClassExpr(const FinishClassExpr &task);
  void walkStaticField(const WalkStaticField &task);
  void finishLateClass(const FinishLateClass &task);
  void walkStaticFields(const WalkStaticFields &task);
  void afterStaticField(const AfterStaticField &task);
  void afterLateClassExpr(const AfterLateClassExpr &task);
  void checkConstructorArgs(const CheckConstructorArgs &task);
  void resolveConstructorType(const ResolveConstructorType &task);
  void afterConstructorTypeArgument(const AfterConstructorTypeArgument &task);

private:
  using ResultBaseClassUsable = Result<ClassDecl *, CodeClassesRegistryBaseClassError>;
  using ResultStaticTypeKnown = Result<const Type *, CodeClassesRegistryStaticTypeError>;
  using ResultClassDeclared = Result<ClassDecl *, CodeClassesRegistryCheckClassStmtError>;
  using ResultConstructedObject = Result<const Type *, CodeClassesRegistryConstructorTypeError>;
  using ResultConstructorArgsList = Result<const Type *, CodeClassesRegistryConstructorArgsError>;
  using ResultConstructorArgument =
      Result<const TypeRef *, CodeClassesRegistryConstructorTypeError>;

  void startLateClass(ClassDecl *classDecl);
  void registerLateClass(ClassDecl *classDecl);
  void rememberLateScope(ClassDecl *classDecl);
  void markClassOverrides(ClassDecl *classDecl);
  void nameAnonymousClass(ClassDecl *classDecl) const;
  void rememberMethodModules(const ClassDecl *classDecl);
  void attachLateParent(ClassDecl *classDecl, ClassDecl *parent);


  ResultBaseClassUsable baseClass(const Type *type) const;
  ResultStaticTypeKnown staticType(const ClassField &field, const Type *type) const;


  ResultConstructorArgsList constructorArgsList() const;
  ResultConstructorArgument checkConstructorTypeArgument(const TypeRef &ref) const;


  ClassDecl *lateParent(ClassDecl *classDecl);
  const FunctionDecl *innerGenericCopy() const;
  const Type *declaredStaticType(const ClassField &field) const;
  ResultClassDeclared checkTopLevelClass(ClassDecl *classDecl) const;
  ResultConstructedObject checkConstructedObject(const Type *made) const;
  std::vector<FunctionDecl *> memberFunctions(const ClassDecl *classDecl) const;

  ClassFacts &facts;
  SemanticState &state;
};

}
