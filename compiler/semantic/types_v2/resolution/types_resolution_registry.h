#pragma once

#include "compiler/program/library/builtin_names/type_name_table/type_name_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include "compiler/semantic/types_v2/resolution/types_resolution_registry_errors.h"
#include "compiler/union/union/union.h"

namespace tz {

class TypesResolutionRegistry {
public:
  using ResultUtilityAllowed = Result<void, TypesResolutionRegistryResolveUtilityTypeError>;

  explicit TypesResolutionRegistry(SemanticState &owner);

  void evalTypeOp(const EvalTypeOp &task);
  void finishTypeUse(const FinishTypeUse &task);
  void finishEnumType(const FinishEnumType &task);
  void finishIntlType(const FinishIntlType &task);
  void resolveTypeRef(const ResolveTypeRef &task);
  void finishAliasType(const FinishAliasType &task);
  void finishArrayType(const FinishArrayType &task);
  void finishTupleType(const FinishTupleType &task);
  void nextObjectField(const NextObjectField &task);
  void nextUnionMember(const NextUnionMember &task);
  void afterObjectField(const AfterObjectField &task);
  void afterUnionMember(const AfterUnionMember &task);
  void finishDescriptor(const FinishDescriptor &task);
  void finishEnumMember(const FinishEnumMember &task);
  void nextTypeArgument(const NextTypeArgument &task);
  void afterTypeArgument(const AfterTypeArgument &task);
  void finishAssertsType(const FinishAssertsType &task);
  void nextFunctionParam(const NextFunctionParam &task);
  void resolveTypeAnswer(const ResolveTypeAnswer &task);
  void writeResolvedType(const WriteResolvedType &task);
  void afterFunctionParam(const AfterFunctionParam &task);
  void finishFunctionType(const FinishFunctionType &task);
  void finishInstanceType(const FinishInstanceType &task);
  void nextUtilityOperand(const NextUtilityOperand &task);
  void afterUtilityOperand(const AfterUtilityOperand &task);
  void resolveInnerTypeRef(const ResolveInnerTypeRef &task);
  void resolveObjectFields(const ResolveObjectFields &task);
  void afterObjectSignature(const AfterObjectSignature &task);
  void afterUtilitySignature(const AfterUtilitySignature &task);

private:
  using ResultParamOrderIsKept = Result<bool, TypesResolutionRegistryParamOrderError>;
  using ResultNonVoidUnionPart = Result<const Type *, TypesResolutionRegistryUnionPartError>;
  using ResultRestParamIsArray = Result<const Type *, TypesResolutionRegistryRestParamError>;
  using ResultStandaloneUsable = Result<const Type *, TypesResolutionRegistryStandaloneError>;
  using ResultTupleLengthFixed = Result<const Type *, TypesResolutionRegistryFixedLengthError>;
  using ResultUniqueFieldNames = Result<llvm::StringRef, TypesResolutionRegistryNewFieldError>;
  using ResultArrayElementType = Result<const Type *, TypesResolutionRegistryArrayElementError>;
  using ResultInferTypeBinding = Result<const Type *, TypesResolutionRegistryInferBindingError>;
  using ResultTupleElementType = Result<const Type *, TypesResolutionRegistryTupleElementError>;
  using ResultTupleSpreadJoins = Result<const Type *, TypesResolutionRegistrySpreadJoinedError>;
  using ResultUnionIsSupported = Result<const Type *, TypesResolutionRegistryUnionAllowedError>;
  using ResultUniqueSignatures = Result<const Type *, TypesResolutionRegistryNewSignatureError>;
  using ResultInstanceOfAClass = Result<const Type *, TypesResolutionRegistryClassInstanceError>;
  using ResultArrayBufferGiven = Result<const Type *, TypesResolutionRegistryBufferArgumentError>;
  using ResultElementTypeGiven = Result<const TypeRef *, TypesResolutionRegistryArrayArgumentError>;
  using ResultEnumMemberExists = Result<const EnumMember *, TypesResolutionRegistryEnumMemberError>;
  using ResultParamTypesAsList = Result<const Type *, TypesResolutionRegistryDescriptorParamsError>;
  using ResultGenericClassUsed =
      Result<const ClassDecl *, TypesResolutionRegistryClassGenericError>;
  using ResultOverloadsAllowed =
      Result<llvm::StringRef, TypesResolutionRegistryOverloadAllowedError>;
  using ResultTwoTypeArguments =
      Result<const TypeRef *, TypesResolutionRegistryDescriptorArityError>;
  using ResultGenericAliasUsed =
      Result<const TypeAliasDecl *, TypesResolutionRegistryAliasGenericError>;

  void pushArray(const TypeRef &ref);
  void startIntl(const TypeRef &ref);
  void startName(const TypeRef &ref);
  void startTuple(const TypeRef &ref);
  void startUnion(const TypeRef &ref);
  void startObject(const TypeRef &ref);
  void startGeneric(const TypeRef &ref);
  void startInnerType(const TypeRef &ref);
  void startOtherName(const TypeRef &ref);
  void startDescriptor(const TypeRef &ref);
  void finishUnion(const UnionDraft &draft);
  void startArrayGeneric(const TypeRef &ref);
  void startFunctionType(const TypeRef &ref);
  void finishObject(const ObjectDraft &draft);
  void startReturned(const TypeRef &returned);
  void startLibraryGeneric(const TypeRef &ref);
  void finishUtility(const UtilityDraft &draft);
  void startDeclared(const TypeRef &ref, Stmt *decl);
  void finishTypeArguments(const TypeArgumentsDraft &draft);
  void reportAsserts(const TypeRef &returned, const Type *type);
  void addUtilityOperand(UtilityDraft draft, const Type *operand);
  void pushObjectMember(const ObjectDraft &draft, bool isSignature);
  void startUtility(const TypeRef &ref, const UtilityTypeName *utility);
  void addOptionalParam(FunctionTypeDraft &draft, const Type *type) const;
  void pushReadyTypeOp(TypeOp kind, const std::vector<const Type *> &operands);
  void startTypeArguments(const TypeRef &ref, Stmt *decl, TypeArgumentsUse use);
  void pushDeferredSpread(const TypeRef &ref, llvm::ArrayRef<const Type *> elements);
  void addRestParam(FunctionTypeDraft &draft, const TypeRef &param, const Type *type);
  void finishSpreadTuple(const TypeRef &ref, const std::vector<const Type *> &elements);
  void addRequiredParam(FunctionTypeDraft &draft, const TypeRef &param, const Type *type);


  bool isNullRef(const TypeRef &ref) const;
  bool hasThisParam(const TypeRef &ref) const;
  bool isBoundParam(const TypeRef &ref) const;
  bool isDictName(llvm::StringRef name) const;
  bool isPromiseName(llvm::StringRef name) const;
  bool isIteratorName(llvm::StringRef name) const;
  bool isMethodAt(const TypeRef &ref, size_t index) const;
  bool isOverloaded(const TypeRef &ref, size_t index) const;
  bool hasErrorType(llvm::ArrayRef<const Type *> types) const;
  bool isMentioningAnyParam(llvm::ArrayRef<const Type *> operands) const;
  bool hasFieldNamed(llvm::ArrayRef<Field> fields, llvm::StringRef name) const;
  bool isSpreadWaiting(const TypeRef &ref, llvm::ArrayRef<const Type *> elements) const;


  const Type *missingType(const TypeRef &ref);
  const Type *resolveInfer(const TypeRef &ref);
  const Type *rejectPredicate(const TypeRef &ref);
  const Type *constructedBy(const Type *type) const;
  const Type *literalType(const TypeRef &ref) const;
  const Type *builtinNamed(llvm::StringRef name) const;
  const Type *firstNonString(const Type *operand) const;
  const Type *bytesTypeNamed(llvm::StringRef name) const;
  const Type *cryptoTypeNamed(llvm::StringRef name) const;
  const Type *standaloneType(const TypeRef &ref, const Type *type);
  const Type *firstError(llvm::ArrayRef<const Type *> operands) const;
  const Type *resolveTypedArray(const TypeRef &ref, const Type *bytes);


  ResultUtilityAllowed checkTextOperand(const TypeRef &ref, const Type *operand) const;
  ResultUtilityAllowed checkFunctionOperand(const TypeRef &ref, const Type *operand) const;
  ResultUtilityAllowed checkArgumentCount(const TypeRef &ref, const UtilityTypeName &utility) const;
  ResultUtilityAllowed checkOperand(const TypeRef &ref, const UtilityTypeName &utility,
                                    const Type *operand) const;


  ResultStandaloneUsable checkNotVoid(const Type *type) const;
  ResultStandaloneUsable checkNotUndefined(const Type *type) const;


  std::optional<std::string> fieldProblem(const ObjectDraft &draft) const;
  std::optional<std::string> signatureProblem(const ObjectDraft &draft) const;


  ClassDecl *localeClass(llvm::StringRef name) const;
  std::string missingText(llvm::StringRef name) const;
  ResultNonVoidUnionPart unionPart(const Type *type) const;
  ResultRestParamIsArray restParam(const Type *type) const;
  ResultTupleLengthFixed fixedLength(const Type *list) const;
  ResultParamOrderIsKept paramOrder(bool optionalBefore) const;
  ResultInferTypeBinding inferBinding(const TypeRef &ref) const;
  ResultArrayElementType arrayElement(const Type *element) const;
  ResultElementTypeGiven arrayArgument(const TypeRef &ref) const;
  ResultTupleElementType tupleElement(const Type *element) const;
  const UtilityTypeName *findUtility(llvm::StringRef name) const;
  ResultTwoTypeArguments descriptorArity(const TypeRef &ref) const;
  ResultInstanceOfAClass classInstance(const Type *classType) const;
  ResultParamTypesAsList descriptorParams(const Type *params) const;
  std::optional<TypeName> resultTypeNamed(llvm::StringRef name) const;
  EnumDecl *enumOwning(llvm::StringRef owner, llvm::StringRef member) const;
  ResultUnionIsSupported unionAllowed(const Type *type, bool withNull) const;
  ResultArrayBufferGiven bufferArgument(const TypeRef &ref, const Type *bytes) const;
  Overload &overloadNamed(std::vector<Overload> &overloads, llvm::StringRef name) const;
  ResultUniqueFieldNames newField(llvm::ArrayRef<Field> fields, llvm::StringRef name) const;
  ResultGenericAliasUsed aliasGeneric(const TypeAliasDecl *alias, llvm::StringRef name) const;
  ResultGenericClassUsed classGeneric(const ClassDecl *classDecl, llvm::StringRef name) const;
  ResultOverloadsAllowed overloadAllowed(const std::vector<Overload> *overloads,
                                         llvm::StringRef name) const;
  ResultUniqueSignatures newSignature(const Overload &overload, const Type *type,
                                      llvm::StringRef name) const;
  ResultTupleSpreadJoins spreadJoined(llvm::ArrayRef<const Type *> elements,
                                      const std::vector<bool> &spread) const;
  ResultEnumMemberExists enumMember(const EnumDecl *declaredEnum, llvm::StringRef owner,
                                    llvm::StringRef member) const;

  SemanticState &state;
};

}
