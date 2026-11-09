#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/flow/children_first/children_first.h"
#include "compiler/semantic/generics/type_params/type_params_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "compiler/union/union/union.h"

namespace tz {

class SemanticParts;
class TypeResolver;
class ClassDeclarer;
class TypeOperators;
class ReturnInferrer;
class DecoratorChecker;
class AliasInstantiator;
class GenericClassInstantiator;

enum class SubstituteForm {
  TYPE_OPERATOR,
  OPTIONAL,
  ARRAY,
  RESULT,
  FUNCTION,
  ITERATOR,
  DICT,
  UNION,
  PROMISE,
  CONSTRUCTOR,
  ASYNC_ITERATOR,
  DESCRIPTOR,
  CLASS_INSTANCE,
  ALIAS_INSTANCE,
  TUPLE,
  OBJECT,
};

enum class SubstituteStepKind { SUBSTITUTE, BUILD };

struct SubstituteType {
  static constexpr SubstituteStepKind kind = SubstituteStepKind::SUBSTITUTE;
  const Type *type;
  bool voidOk;
};

struct BuildSubstituted {
  static constexpr SubstituteStepKind kind = SubstituteStepKind::BUILD;
  const Type *type;
  SubstituteForm form;
  size_t count;
};

using SubstituteStep = Union<SubstituteType, BuildSubstituted>;

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
  static NodeChildren<const Type *> representationParts(const Type *type);
  static const Type *nodeRepresentation(const Type *type, std::vector<const Type *> &answers);
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
  static bool mentionsInObject(const Type *type, llvm::ArrayRef<const Type *> params);
  bool objectHasMembersOf(const Type *arg, const Type *constraint);

  const Type *substituteAt(const Type *type, llvm::ArrayRef<const Type *> params,
                           llvm::ArrayRef<const Type *> args, bool voidOk);
  void enterSubstitute(const SubstituteType &enter, llvm::ArrayRef<const Type *> params,
                       llvm::ArrayRef<const Type *> args, std::vector<SubstituteStep> &steps,
                       std::vector<const Type *> &answers);
  static const Type *plainSubstitute(const Type *type, llvm::ArrayRef<const Type *> params,
                                     llvm::ArrayRef<const Type *> args, bool voidOk);
  std::optional<SubstituteForm> substituteForm(const Type *type) const;
  std::optional<SubstituteForm> objectForm(const Type *type) const;
  std::vector<SubstituteType> substituteParts(const Type *type, SubstituteForm form) const;
  static std::vector<SubstituteType> partsOf(llvm::ArrayRef<const Type *> types, bool voidOk);
  static std::vector<SubstituteType> fieldParts(const Type *type);
  static bool hasListParam(const Type *tuple);
  const Type *buildSubstituted(const BuildSubstituted &build, std::vector<const Type *> &answers);
  const Type *substitutedInstance(const Type *type, std::vector<const Type *> newArgs);
  static const Type *substitutedTuple(const Type *type, std::vector<const Type *> parts);
  const Type *substitutedObject(const Type *type, llvm::ArrayRef<const Type *> parts);

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
