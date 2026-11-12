#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/generics/generic_instantiator/generic_instantiator_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class Inference;
class TypeParams;
class LateClasses;
class TypeResolver;
class TypeRelations;
class ArgListChecker;
class ReturnInferrer;
class FunctionChecker;
class TypeArgInferrer;
class GenericClassChecker;

class GenericInstantiator {
public:
  GenericInstantiator(SemanticState &owner, SemanticParts &all);

  Expr *checkGenericValue(NameExpr *name, FuncSymbol *generic, const Signature *expected);
  bool returnsOwnClass(const FuncSymbol *generic, const Type *returnType);
  FuncSymbol *instantiate(FuncSymbol *generic, llvm::ArrayRef<const Type *> args, llvm::SMLoc loc,
                          llvm::ArrayRef<const Type *> exact = {});
  std::vector<const Type *> exactClassArgs(CallExpr *call, const FuncSymbol *generic);
  FunctionDecl *instanceDecl(FuncSymbol *generic, llvm::ArrayRef<const Type *> args,
                             llvm::ArrayRef<const Type *> exact = {});
  void walkInstance(InstanceRecord *instance);
  void reportInstance(InstanceRecord *instance, llvm::SMLoc callLoc,
                      llvm::ArrayRef<const Type *> concrete);
  void walkInstances();

private:
  using ResultTypeArgsCount = Result<size_t, GenericInstantiatorTypeArgCountError>;
  using ResultTypeArgsGiven = Result<const NameExpr *, GenericInstantiatorTypeArgsGivenError>;
  using ResultNoClassReturn = Result<const FuncSymbol *, GenericInstantiatorOwnClassReturnError>;
  using ResultChainIsFinite = Result<unsigned, GenericInstantiatorChainBoundedError>;
  using ResultParamsMeaning = Result<const FuncSymbol *, GenericInstantiatorParamsMeaningfulError>;

  ResultTypeArgsCount typeArgCount(const FuncSymbol *generic, const NameExpr *name) const;
  ResultTypeArgsGiven typeArgsGiven(const FuncSymbol *generic, const NameExpr *name,
                                    const Signature *expected) const;
  ResultNoClassReturn ownClassReturn(const FuncSymbol *generic, const ClassDecl *classDecl) const;
  ResultChainIsFinite chainBounded(const FuncSymbol *generic,
                                   llvm::ArrayRef<const Type *> args) const;
  ResultParamsMeaning paramsMeaningful(const FuncSymbol *generic, llvm::ArrayRef<const Type *> reps,
                                       llvm::ArrayRef<const Type *> args);
  bool writtenTypeArgsFit(const NameExpr *name, const FuncSymbol *generic,
                          std::vector<const Type *> &bound);
  void bindFromExpected(const FuncSymbol *generic, const Type *returnType,
                        const Signature &expected, std::vector<const Type *> &bound);
  const Type *unifiedCandidates(const Inference &inference, size_t index) const;
  void typeGenericValue(NameExpr *name, FuncSymbol *generic, const Type *returnType,
                        const std::vector<const Type *> &bound);
  bool isConcrete(llvm::ArrayRef<const Type *> bound) const;
  bool hasErrorArg(llvm::ArrayRef<const Type *> args) const;
  std::vector<const Type *> instanceReps(FuncSymbol *generic, llvm::ArrayRef<const Type *> args);
  FuncSymbol *reuseInstance(InstanceRecord *instance, llvm::SMLoc loc,
                            llvm::ArrayRef<const Type *> args);
  bool instanceArgsFit(const FuncSymbol *generic, llvm::ArrayRef<const Type *> reps,
                       llvm::ArrayRef<const Type *> args, llvm::SMLoc loc);
  void placeInstance(const InstanceRecord &instance);
  FuncSymbol *checkNewInstance(InstanceRecord *instance, llvm::SMLoc loc,
                               llvm::ArrayRef<const Type *> args);
  const ClassDecl *literalClassIn(const Type *type) const;
  void findPreciseError(InstanceRecord *instance);
  void noteFailedChain(const InstanceRecord *start);
  void walkPendingInstance(InstanceRecord *instance);
  unsigned chainLength(const FuncSymbol *generic) const;
  std::vector<const CastExpr *> implicitCasts(const Expr *arg) const;
  std::vector<Diagnostic> walkCopy(InstanceRecord *instance, FunctionDecl *copy);
  bool isGenericClassParam(const FuncSymbol *generic, const Type *param) const;
  std::string failedCallNote(const InstanceRecord *at, const InstanceRecord *next) const;
  std::vector<const Type *> instanceKey(llvm::ArrayRef<const Type *> reps,
                                        llvm::ArrayRef<const Type *> exact) const;
  void takeExactClass(CallExpr *call, size_t index, size_t paramCount,
                      std::vector<const Type *> &exact);
  void applyExactParams(FuncSymbol *function, FunctionDecl *copy,
                        llvm::ArrayRef<const Type *> exact) const;
  FuncSymbol *instanceSymbol(const FuncSymbol *generic, FunctionDecl *copy,
                             llvm::ArrayRef<const Type *> args);
  void substituteCopyParams(const FunctionDecl *original, FunctionDecl *copy,
                            llvm::ArrayRef<const Type *> params, llvm::ArrayRef<const Type *> args);

  SemanticState &state;
  TypeParams *typeParams = nullptr;
  LateClasses *lateClasses = nullptr;
  TypeResolver *typeResolver = nullptr;
  TypeRelations *typeRelations = nullptr;
  ArgListChecker *argListChecker = nullptr;
  ReturnInferrer *returnInferrer = nullptr;
  FunctionChecker *functionChecker = nullptr;
  TypeArgInferrer *typeArgInferrer = nullptr;
  GenericClassChecker *genericClassChecker = nullptr;
};

}
