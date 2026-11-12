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

enum class VoidAs {
  VOID,
  UNDEFINED,
};

struct SubstituteType {
  static constexpr SubstituteStepKind kind = SubstituteStepKind::SUBSTITUTE;
  const Type *type;
  VoidAs voidAs;
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
  const Type *representation(const Type *type) const;
  bool isSatisfying(const Type *arg, const Type *param, llvm::ArrayRef<const Type *> params = {},
                    llvm::ArrayRef<const Type *> args = {});
  bool hasMembersOf(const Type *arg, const Type *constraint);
  bool isExactTypeParam(FuncSymbol *generic, size_t index);

private:
  NodeChildren<const Type *> representationParts(const Type *type) const;
  const Type *nodeRepresentation(const Type *type, std::vector<const Type *> &answers) const;
  using ResultParamNameIsNew = Result<const TypeParamDecl *, TypeParamsUniqueNameError>;
  using ResultNotBuiltInName = Result<const TypeParamDecl *, TypeParamsOwnNameError>;
  using ResultConstraintKept = Result<const Type *, TypeParamsConstraintAloneError>;
  using ResultConstraintsKept = Result<void, TypeParamsConstraintAloneError>;
  using ResultDefaultSatisfy = Result<const Type *, TypeParamsDefaultFitsError>;
  using ResultDefaultPresent = Result<const TypeParamDecl *, TypeParamsDefaultGivenError>;

  struct ParamConstraint {
    const Type *constraint;
    std::vector<const Type *> alternatives;
    bool isResolved;
  };

  std::vector<Type *> createTypeParams(llvm::ArrayRef<TypeParamDecl> decls);
  void constrainTypeParam(Type *param, const TypeParamDecl &typeParam);
  ParamConstraint readConstraint(const TypeRef &ref, Type *param);
  ParamConstraint unionConstraint(const TypeRef &ref);
  ResultConstraintsKept checkConstraintsAlone(const ParamConstraint &found,
                                              const TypeParamDecl &typeParam) const;
  ParamConstraint noConstraint() const;
  bool isUnknownList(const TypeRef &ref) const;
  void walkParamName(llvm::ArrayRef<TypeParamDecl> decls, size_t index);
  const Type *resolvedDefault(const TypeParamDecl &typeParam, const Type *param);
  ResultNotBuiltInName ownName(const TypeParamDecl &typeParam) const;
  ResultConstraintKept constraintAlone(const Type *type, const TypeParamDecl &typeParam) const;
  ResultDefaultPresent defaultGiven(const TypeParamDecl &typeParam, bool isAfterDefault) const;
  ResultParamNameIsNew uniqueName(const TypeParamDecl &earlier,
                                  const TypeParamDecl &typeParam) const;
  ResultDefaultSatisfy defaultFits(const Type *fallback, const Type *param,
                                   const TypeParamDecl &typeParam);
  bool isMentioningInObject(const Type *type, llvm::ArrayRef<const Type *> params) const;
  bool hasObjectMembersOf(const Type *arg, const Type *constraint);
  bool isTextUnion(const Type *type) const;
  bool isObjectLike(const Type *type) const;
  bool hasObjectLikeAlternative(const Type *param) const;
  std::vector<bool> exactParams(const FuncSymbol *generic) const;
  bool isExactParam(const FuncSymbol *generic, size_t index) const;
  bool isFittingConstraint(const Type *arg, const Type *constraint);
  bool isMatchingField(const Field &have, const Field &wanted) const;
  bool hasMethodField(const ClassDecl *classDecl, const Field &field);
  bool hasMatchingOverload(const ClassDecl *classDecl, const Overload &overload);
  bool hasMethodOfType(llvm::ArrayRef<FunctionDecl *> methods, const Type *signature);
  bool hasMatchingField(const Type *arg, const ClassDecl *classDecl, const Field &field);
  const Type *resultRepresentation(std::vector<const Type *> &answers) const;
  const Type *unionRepresentation(const Type *type, std::vector<const Type *> &answers) const;

  const Type *substituteAt(const Type *type, llvm::ArrayRef<const Type *> params,
                           llvm::ArrayRef<const Type *> args, VoidAs voidAs);
  void enterSubstitute(const SubstituteType &enter, llvm::ArrayRef<const Type *> params,
                       llvm::ArrayRef<const Type *> args, std::vector<SubstituteStep> &steps,
                       std::vector<const Type *> &answers);
  const Type *plainSubstitute(const Type *type, llvm::ArrayRef<const Type *> params,
                              llvm::ArrayRef<const Type *> args, VoidAs voidAs) const;
  std::optional<SubstituteForm> substituteForm(const Type *type) const;
  std::optional<SubstituteForm> paramForm(const Type *type) const;
  std::optional<SubstituteForm> classRefForm(const Type *type) const;
  std::optional<SubstituteForm> objectForm(const Type *type) const;
  std::vector<SubstituteType> substituteParts(const Type *type, SubstituteForm form) const;
  std::vector<SubstituteType> partsOf(llvm::ArrayRef<const Type *> types, VoidAs voidAs) const;
  SubstituteType valuePart(const Type *type) const;
  SubstituteType voidPart(const Type *type) const;
  std::vector<SubstituteType> functionParts(const Type *type) const;
  std::vector<SubstituteType> descriptorParts(const Type *type) const;
  std::vector<SubstituteType> aliasParts(const Type *type) const;
  std::vector<SubstituteType> tupleParts(const Type *type) const;
  const Type *substitutedFunction(const Type *type, std::vector<const Type *> parts);
  const Type *substitutedAlias(const Type *type, std::vector<const Type *> parts);
  std::vector<SubstituteType> fieldParts(const Type *type) const;
  bool hasListParam(const Type *tuple) const;
  const Type *buildSubstituted(const BuildSubstituted &build, std::vector<const Type *> &answers);
  const Type *substitutedInstance(const Type *type, std::vector<const Type *> newArgs);
  const Type *substitutedTuple(const Type *type, std::vector<const Type *> parts) const;
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
