#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/generics/generic_class_checker/generic_class_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class ClassLinks;
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

  void declareClassTypeParams(ClassDecl *classDecl);
  void bindClassTypeParams(const ClassDecl *classDecl);
  const Type *instanceType(const ClassDecl *instance, const Type *type);
  const Type *instanceValueType(const ClassDecl *instance, const Type *type);
  void walkClassInstance(InstanceRecord *instance);
  void reportClassInstance(InstanceRecord *instance, llvm::SMLoc loc);
  Expr *checkGenericNew(NewExpr *expr, ClassDecl *generic, const Type *expected);

private:
  using ResultClassDeclared = Result<const ClassDecl *, GenericClassCheckerDeclaredFirstError>;
  using ResultClassConcrete = Result<const ClassDecl *, GenericClassCheckerConcreteError>;
  using ResultCtorWasPicked = Result<FunctionDecl *, GenericClassCheckerConstructorFitError>;

  ResultClassConcrete concrete(const ClassDecl *generic) const;
  ResultClassDeclared declaredFirst(const ClassDecl *generic) const;
  ResultCtorWasPicked constructorFit(const NewExpr *expr, const ClassDecl *generic,
                                     llvm::ArrayRef<FunctionDecl *> constructors);
  void noteFailedChain(const InstanceRecord *start);
  bool isTakingCount(const FuncSymbol *function, size_t count) const;
  std::vector<Diagnostic> walkInstanceBodies(InstanceRecord *instance);
  std::vector<FunctionDecl *> instanceBodies(const ClassDecl *classDecl) const;
  std::vector<Diagnostic> walkInstanceBody(InstanceRecord *instance, FunctionDecl *functionDecl);
  std::string failedCallNote(const InstanceRecord *at, const InstanceRecord *next) const;
  void walkArgsLoosely(std::vector<Expr *> &args);
  Expr *checkLibraryGenericNew(NewExpr *expr, ClassDecl *generic, const Type *expected);
  Expr *checkExplicitGenericNew(NewExpr *expr, ClassDecl *generic, const Type *expected);
  ClassDecl *instantiatedClass(const NewExpr *expr, ClassDecl *generic,
                               std::vector<const Type *> args);
  std::optional<FunctionDecl *> pickedConstructor(const NewExpr *expr, const ClassDecl *generic,
                                                  llvm::ArrayRef<FunctionDecl *> constructors);
  FuncSymbol *constructorFunction(ClassDecl *generic, FunctionDecl *constructor);
  Expr *finishGenericNew(NewExpr *expr, ClassDecl *instance, CallExpr *call,
                         FunctionDecl *constructor, llvm::ArrayRef<FunctionDecl *> constructors);
  void linkInstanceConstructor(const NewExpr *expr, ClassDecl *instance, CallExpr *call,
                               FunctionDecl *constructor,
                               llvm::ArrayRef<FunctionDecl *> constructors);
  bool hasInitializer(const ClassDecl *instance) const;

  SemanticState &state;
  ChunkTypes *chunkTypes = nullptr;
  TypeParams *typeParams = nullptr;
  ClassLinks *classLinks = nullptr;
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
