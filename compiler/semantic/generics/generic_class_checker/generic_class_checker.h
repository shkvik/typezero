#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/generics/generic_class_checker/generic_class_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class ChunkTypes;
class TypeParams;
class ExprChecker;
class FlowChecker;
class TypeResolver;
class ClassDeclarer;
class EmitterChecker;
class ModuleDeclarer;
class ReturnInferrer;
class FunctionChecker;
class TypeArgInferrer;
class StreamNewChecker;
class MethodCallChecker;
class ClassMemberChecker;
class GenericClassInstantiator;

class GenericClassChecker {
public:
  static const ClassDecl *genericInstanceOf(const Type *type, const ClassDecl *generic);
  GenericClassChecker(SemanticState &owner, SemanticParts &all);

  void declareClassTypeParams(ClassDecl *cls);
  void bindClassTypeParams(const ClassDecl *cls);
  const Type *instanceType(const ClassDecl *inst, const Type *type);
  const Type *instanceValueType(const ClassDecl *inst, const Type *type);
  void walkClassInstance(InstanceRecord *instance);
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
  void walkArgsLoosely(std::vector<Expr *> &args);
  Expr *checkLibraryGenericNew(NewExpr *expr, ClassDecl *generic, const Type *expected);
  Expr *checkExplicitGenericNew(NewExpr *expr, ClassDecl *generic, const Type *expected);
  ClassDecl *instantiatedClass(const NewExpr *expr, ClassDecl *generic,
                               std::vector<const Type *> args);
  std::optional<FunctionDecl *> pickedConstructor(const NewExpr *expr, const ClassDecl *generic,
                                                  llvm::ArrayRef<FunctionDecl *> ctors);
  FuncSymbol *constructorFunction(ClassDecl *generic, FunctionDecl *ctor);
  Expr *finishGenericNew(NewExpr *expr, ClassDecl *inst, CallExpr *call, FunctionDecl *ctor,
                         llvm::ArrayRef<FunctionDecl *> ctors);
  void linkInstanceConstructor(const NewExpr *expr, ClassDecl *inst, CallExpr *call,
                               FunctionDecl *ctor, llvm::ArrayRef<FunctionDecl *> ctors);
  static bool hasInitializer(const ClassDecl *inst);

  SemanticState &state;
  ChunkTypes *chunkTypes = nullptr;
  TypeParams *typeParams = nullptr;
  ExprChecker *exprChecker = nullptr;
  FlowChecker *flowChecker = nullptr;
  TypeResolver *typeResolver = nullptr;
  ClassDeclarer *classDeclarer = nullptr;
  EmitterChecker *emitterChecker = nullptr;
  ModuleDeclarer *moduleDeclarer = nullptr;
  ReturnInferrer *returnInferrer = nullptr;
  FunctionChecker *functionChecker = nullptr;
  TypeArgInferrer *typeArgInferrer = nullptr;
  StreamNewChecker *streamNewChecker = nullptr;
  MethodCallChecker *methodCallChecker = nullptr;
  ClassMemberChecker *classMemberChecker = nullptr;
  GenericClassInstantiator *genericClassInstantiator = nullptr;
};

}
