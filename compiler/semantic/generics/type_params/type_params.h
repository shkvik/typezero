#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/generics/type_params/type_params_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class TypeResolver;
class ClassDeclarer;
class TypeOperators;
class ReturnInferrer;
class DecoratorChecker;
class AliasInstantiator;
class GenericClassInstantiator;

class TypeParams {
public:
  TypeParams(SemanticState &owner, SemanticParts &all);

  std::vector<const Type *> declareTypeParams(llvm::ArrayRef<TypeParamDecl> decls,
                                              std::vector<const Type *> *defaults);
  void bindTypeParams(llvm::ArrayRef<TypeParamDecl> decls, llvm::ArrayRef<const Type *> types);
  const Type *substitute(const Type *type, llvm::ArrayRef<const Type *> params,
                         llvm::ArrayRef<const Type *> args);
  const Type *substituteValue(const Type *type, llvm::ArrayRef<const Type *> params,
                              llvm::ArrayRef<const Type *> args);
  static const Type *representation(const Type *type);
  bool satisfies(const Type *arg, const Type *param, llvm::ArrayRef<const Type *> params = {},
                 llvm::ArrayRef<const Type *> args = {});
  bool hasMembersOf(const Type *arg, const Type *constraint);
  bool exactTypeParam(FuncSymbol *generic, size_t k);

private:
  using ResultParamNameIsNew = Result<const TypeParamDecl *, TypeParamsUniqueNameError>;
  using ResultNotBuiltInName = Result<const TypeParamDecl *, TypeParamsOwnNameError>;
  using ResultConstraintKept = Result<const Type *, TypeParamsConstraintAloneError>;
  using ResultDefaultSatisfy = Result<const Type *, TypeParamsDefaultFitsError>;
  using ResultDefaultPresent = Result<const TypeParamDecl *, TypeParamsDefaultGivenError>;

  static ResultNotBuiltInName ownName(const TypeParamDecl &decl);
  static ResultConstraintKept constraintAlone(const Type *type, const TypeParamDecl &decl);
  static ResultDefaultPresent defaultGiven(const TypeParamDecl &decl, bool afterDefault);
  static ResultParamNameIsNew uniqueName(const TypeParamDecl &earlier, const TypeParamDecl &decl);
  ResultDefaultSatisfy defaultFits(const Type *fallback, const Type *param,
                                   const TypeParamDecl &decl);
  static bool mentionsInObject(const Type *type, llvm::ArrayRef<const Type *> params,
                               llvm::SmallPtrSetImpl<const Type *> &seen);

  const Type *substituteAt(const Type *type, llvm::ArrayRef<const Type *> params,
                           llvm::ArrayRef<const Type *> args, bool voidOk);

  SemanticState &state;
  TypeResolver *typeResolver = nullptr;
  ClassDeclarer *classDeclarer = nullptr;
  TypeOperators *typeOperators = nullptr;
  ReturnInferrer *returnInferrer = nullptr;
  DecoratorChecker *decoratorChecker = nullptr;
  AliasInstantiator *aliasInstantiator = nullptr;
  GenericClassInstantiator *genericClassInstantiator = nullptr;
};

}
