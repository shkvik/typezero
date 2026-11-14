#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/generics_v2/parameters/generics_parameters_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class GenericsParametersRegistry {
public:
  explicit GenericsParametersRegistry(SemanticState &owner);

  void nextTypeParam(const NextTypeParam &task);
  void startTypeParams(const StartTypeParams &task);
  void afterDefaultType(const AfterDefaultType &task);
  void afterConstraintType(const AfterConstraintType &task);
  void bindClassTypeParams(const BindClassTypeParams &task);
  void afterAliasTypeParams(const AfterAliasTypeParams &task);
  void afterClassTypeParams(const AfterClassTypeParams &task);
  void leaveClassTypeParams(const LeaveClassTypeParams &task);
  void nextConstraintMember(const NextConstraintMember &task);
  void afterConstraintMember(const AfterConstraintMember &task);
  void afterDefaultConstraint(const AfterDefaultConstraint &task);
  void bindFunctionTypeParams(const BindFunctionTypeParams &task);
  void declareAliasTypeParams(const DeclareAliasTypeParams &task);
  void declareClassTypeParams(const DeclareClassTypeParams &task);
  void afterFunctionTypeParams(const AfterFunctionTypeParams &task);
  void declareFunctionTypeParams(const DeclareFunctionTypeParams &task);

private:
  using ResultConstraintsKept = Result<void, GenericsParametersRegistryConstraintAloneError>;
  using ResultDefaultSatisfy = Result<const Type *, GenericsParametersRegistryDefaultFitsError>;
  using ResultConstraintKept = Result<const Type *, GenericsParametersRegistryConstraintAloneError>;
  using ResultNotBuiltInName =
      Result<const TypeParamDecl *, GenericsParametersRegistryOwnNameError>;
  using ResultParamNameIsNew =
      Result<const TypeParamDecl *, GenericsParametersRegistryUniqueNameError>;
  using ResultDefaultPresent =
      Result<const TypeParamDecl *, GenericsParametersRegistryDefaultGivenError>;

  void startDefault(TypeParamsDraft draft);
  void applyConstraint(TypeParamsDraft draft);
  void finishTypeParam(TypeParamsDraft draft);
  void walkParamName(llvm::ArrayRef<TypeParamDecl> decls, size_t index);
  void bindTypeParams(llvm::ArrayRef<TypeParamDecl> decls, llvm::ArrayRef<const Type *> types);


  bool isUnknownList(const TypeRef &ref) const;


  ResultNotBuiltInName ownName(const TypeParamDecl &typeParam) const;
  std::vector<const Type *> paramsOf(const TypeParamsDraft &draft) const;
  std::vector<Type *> createTypeParams(llvm::ArrayRef<TypeParamDecl> decls);
  ResultConstraintKept constraintAlone(const Type *type, const TypeParamDecl &typeParam) const;
  ResultDefaultPresent defaultGiven(const TypeParamDecl &typeParam, bool isAfterDefault) const;
  ResultParamNameIsNew uniqueName(const TypeParamDecl &earlier,
                                  const TypeParamDecl &typeParam) const;
  ResultConstraintsKept checkConstraintsAlone(const TypeParamsDraft &draft,
                                              const TypeParamDecl &typeParam) const;

  ResultDefaultSatisfy defaultFits(const Type *fallback, const Type *param,
                                   const TypeParamDecl &typeParam, bool isSatisfied) const;

  SemanticState &state;
};

}
