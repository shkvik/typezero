#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/generics/generic_class_checker/generic_class_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class GenericClassChecker {
public:
  static const ClassDecl *genericInstanceOf(const Type *type, const ClassDecl *generic);
  explicit GenericClassChecker(SemanticState &owner);

  void declareClassTypeParams(ClassDecl *cls);
  void bindClassTypeParams(const ClassDecl *cls);
  const Type *instanceType(const ClassDecl *inst, const Type *type);
  const Type *instanceValueType(const ClassDecl *inst, const Type *type);
  void checkClassInstance(InstanceRecord *instance);
  void reportClassInstance(InstanceRecord *instance, llvm::SMLoc loc);
  Expr *checkGenericNew(NewExpr *expr, ClassDecl *generic, const Type *expected);

private:
  using ResultClassDeclared = Result<const ClassDecl *, GenericClassCheckerDeclaredFirstError>;
  using ResultClassConcrete = Result<const ClassDecl *, GenericClassCheckerConcreteError>;
  using ResultCtorWasPicked = Result<FunctionDecl *, GenericClassCheckerConstructorFitError>;

  static ResultClassConcrete concrete(const ClassDecl *generic);
  ResultClassDeclared declaredFirst(const ClassDecl *generic) const;
  ResultCtorWasPicked constructorFit(const NewExpr *expr, const ClassDecl *generic,
                                     llvm::ArrayRef<FunctionDecl *> ctors);
  static std::string typeNames(llvm::ArrayRef<const Type *> types);
  static std::string bindings(llvm::ArrayRef<const Type *> params,
                              llvm::ArrayRef<const Type *> args);

  SemanticState &state;
};

}
