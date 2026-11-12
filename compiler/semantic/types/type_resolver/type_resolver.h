#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "compiler/semantic/types/type_resolver/type_resolver_errors.h"

namespace tz {

class IntlChecker;
class BytesChecker;
class SemanticParts;
class DictChecker;
class ResultTypes;
class AsyncChecker;
class TypeDeclarer;
class UtilityTypes;
class CryptoChecker;
class TemplateTypes;
class TypeOperators;
class ConditionalTypes;
class DecoratorChecker;
class GeneratorChecker;
class AliasInstantiator;
class ClassValueChecker;
class BuiltinModuleChecker;
class GenericClassInstantiator;

enum class TypeOperatorsResolveTypeOpErrorCode;

class TypeResolver {
public:
  TypeResolver(SemanticState &owner, SemanticParts &all);

  const Type *resolveType(const TypeRef &ref);
  const Type *resolveTypeInner(const TypeRef &ref);
  const Type *resolveTypeOrVoid(const TypeRef &ref);
  std::vector<Field> resolveFields(const TypeRef &ref, std::vector<Overload> *overloads = nullptr);

private:
  struct FunctionParams {
    std::vector<const Type *> types;
    size_t minArgs;
    bool hasOptional;
    bool isValid;
  };

  using ResultParamOrderIsKept = Result<bool, TypeResolverParamOrderError>;
  using ResultIntlTypeResolved = Result<const Type *, TypeResolverIntlTypeError>;
  using ResultRestParamIsArray = Result<const Type *, TypeResolverRestParamError>;
  using ResultNonVoidUnionPart = Result<const Type *, TypeResolverUnionPartError>;
  using ResultStandaloneUsable = Result<const Type *, TypeResolverStandaloneError>;
  using ResultTupleLengthFixed = Result<const Type *, TypeResolverFixedLengthError>;
  using ResultUniqueFieldNames = Result<llvm::StringRef, TypeResolverNewFieldError>;
  using ResultInferTypeBinding = Result<const Type *, TypeResolverInferBindingError>;
  using ResultArrayElementType = Result<const Type *, TypeResolverArrayElementError>;
  using ResultUnionIsSupported = Result<const Type *, TypeResolverUnionAllowedError>;
  using ResultTupleElementType = Result<const Type *, TypeResolverTupleElementError>;
  using ResultTupleSpreadJoins = Result<const Type *, TypeResolverSpreadJoinedError>;
  using ResultUniqueSignatures = Result<const Type *, TypeResolverNewSignatureError>;
  using ResultInstanceOfAClass = Result<const Type *, TypeResolverClassInstanceError>;
  using ResultArrayBufferGiven = Result<const Type *, TypeResolverBufferArgumentError>;
  using ResultParamTypesAsList = Result<const Type *, TypeResolverDescriptorParamsError>;
  using ResultElementTypeGiven = Result<const TypeRef *, TypeResolverArrayArgumentError>;
  using ResultEnumMemberExists = Result<const EnumMember *, TypeResolverEnumMemberError>;
  using ResultGenericClassUsed = Result<const ClassDecl *, TypeResolverClassGenericError>;
  using ResultTwoTypeArguments = Result<const TypeRef *, TypeResolverDescriptorArityError>;
  using ResultOverloadsAllowed = Result<llvm::StringRef, TypeResolverOverloadAllowedError>;
  using ResultGenericAliasUsed = Result<const TypeAliasDecl *, TypeResolverAliasGenericError>;

  ResultNonVoidUnionPart unionPart(const Type *type) const;
  ResultRestParamIsArray restParam(const Type *type) const;
  ResultTupleLengthFixed fixedLength(const Type *list) const;
  ResultStandaloneUsable checkNotVoid(const Type *type) const;
  ResultIntlTypeResolved intlType(llvm::StringRef name) const;
  ResultParamOrderIsKept paramOrder(bool optionalBefore) const;
  ResultInferTypeBinding inferBinding(const TypeRef &ref) const;
  ResultArrayElementType arrayElement(const Type *element) const;
  ResultTupleElementType tupleElement(const Type *element) const;
  ResultElementTypeGiven arrayArgument(const TypeRef &ref) const;
  ResultTwoTypeArguments descriptorArity(const TypeRef &ref) const;
  ResultStandaloneUsable checkNotUndefined(const Type *type) const;
  ResultInstanceOfAClass classInstance(const Type *classType) const;
  ResultParamTypesAsList descriptorParams(const Type *params) const;
  ResultUnionIsSupported unionAllowed(const Type *type, bool withNull) const;
  ResultArrayBufferGiven bufferArgument(const TypeRef &ref, const Type *bytes) const;
  ResultUniqueFieldNames newField(llvm::ArrayRef<Field> fields, llvm::StringRef name) const;
  ResultGenericClassUsed classGeneric(const ClassDecl *classDecl, llvm::StringRef name) const;
  ResultGenericAliasUsed aliasGeneric(const TypeAliasDecl *alias, llvm::StringRef name) const;
  ResultOverloadsAllowed overloadAllowed(const std::vector<Overload> *overloads,
                                         llvm::StringRef name) const;
  ResultUniqueSignatures newSignature(const Overload &overload, const Type *type,
                                      llvm::StringRef name) const;
  ResultTupleSpreadJoins spreadJoined(llvm::ArrayRef<const Type *> elements,
                                      const std::vector<bool> &spread) const;
  ResultEnumMemberExists enumMember(const EnumDecl *declaredEnum, llvm::StringRef owner,
                                    llvm::StringRef member) const;

  bool isNullRef(const TypeRef &ref) const;
  bool isBoundParam(const TypeRef &ref) const;
  bool hasThisParam(const TypeRef &ref) const;
  const Type *resolveName(const TypeRef &ref);
  const Type *resolveIntl(const TypeRef &ref);
  const Type *resolveDict(const TypeRef &ref);
  const Type *missingType(const TypeRef &ref);
  const Type *resolveArray(const TypeRef &ref);
  const Type *resolveUnion(const TypeRef &ref);
  const Type *resolveTuple(const TypeRef &ref);
  const Type *resolveInfer(const TypeRef &ref);
  const Type *resolveObject(const TypeRef &ref);
  const Type *resolveGeneric(const TypeRef &ref);
  const Type *resolvePromise(const TypeRef &ref);
  const Type *resolveUtility(const TypeRef &ref);
  const Type *rejectPredicate(const TypeRef &ref);
  const Type *resolveOperator(const TypeRef &ref);
  const Type *resolveIterator(const TypeRef &ref);
  FunctionParams resolveParams(const TypeRef &ref);
  const Type *literalType(const TypeRef &ref) const;
  const Type *resolveDescriptor(const TypeRef &ref);
  ClassDecl *localeClass(llvm::StringRef name) const;
  const Type *rejectAsserts(const TypeRef &returned);
  const Type *resolveConstructor(const TypeRef &ref);
  std::string missingText(llvm::StringRef name) const;
  const Type *resolveArrayGeneric(const TypeRef &ref);
  const Type *resolveInstanceType(const TypeRef &ref);
  const Type *resolveIntersection(const TypeRef &ref);
  const Type *resolveFunctionType(const TypeRef &ref);
  const Type *builtinNamed(llvm::StringRef name) const;
  const Type *resolveReturned(const TypeRef &returned);
  const Type *resolveConstructorArgs(const TypeRef &ref);
  bool isMethodAt(const TypeRef &ref, size_t index) const;
  bool isOverloaded(const TypeRef &ref, size_t index) const;
  bool hasErrorType(llvm::ArrayRef<const Type *> types) const;
  const Type *resolveParam(const TypeRef &param, bool isRest);
  const Type *resolveDeclared(const TypeRef &ref, Stmt *decl);
  const Type *standaloneType(const TypeRef &ref, const Type *type);
  const Type *resolveTypedArray(const TypeRef &ref, const Type *bytes);
  void addOptionalParam(FunctionParams &params, const Type *type) const;
  EnumDecl *enumOwning(llvm::StringRef owner, llvm::StringRef member) const;
  const Type *resolveUnionMember(const TypeRef &ref, const TypeRef &member);
  const Type *resolveEnumMember(const TypeRef &ref, EnumDecl *declaredEnum);
  const Type *resolveGenericClass(const TypeRef &ref, ClassDecl *classDecl);
  const Type *resolveGenericAlias(const TypeRef &ref, TypeAliasDecl *alias);
  void addField(const TypeRef &ref, size_t index, std::vector<Field> &fields);
  bool hasFieldNamed(llvm::ArrayRef<Field> fields, llvm::StringRef name) const;
  void addRestParam(FunctionParams &params, const TypeRef &param, const Type *type);
  Overload &overloadNamed(std::vector<Overload> &overloads, llvm::StringRef name) const;
  bool isSpreadWaiting(const TypeRef &ref, llvm::ArrayRef<const Type *> elements) const;
  void addSignature(const TypeRef &ref, size_t index, std::vector<Overload> *overloads);
  const Type *deferredSpread(const TypeRef &ref, llvm::ArrayRef<const Type *> elements);
  void addRequiredParam(FunctionParams &params, const TypeRef &param, const Type *type);
  const Type *resolveSpreadTuple(const TypeRef &ref, const std::vector<const Type *> &elements);
  const Type *instantiate(ClassDecl *classDecl, std::vector<const Type *> args, llvm::SMLoc loc);
  llvm::SMLoc operatorErrorLoc(const TypeRef &ref, TypeOperatorsResolveTypeOpErrorCode code) const;

  SemanticState &state;
  DictChecker *dictChecker = nullptr;
  ResultTypes *resultTypes = nullptr;
  IntlChecker *intlChecker = nullptr;
  AsyncChecker *asyncChecker = nullptr;
  TypeDeclarer *typeDeclarer = nullptr;
  UtilityTypes *utilityTypes = nullptr;
  BytesChecker *bytesChecker = nullptr;
  CryptoChecker *cryptoChecker = nullptr;
  TemplateTypes *templateTypes = nullptr;
  TypeOperators *typeOperators = nullptr;
  ConditionalTypes *conditionalTypes = nullptr;
  DecoratorChecker *decoratorChecker = nullptr;
  GeneratorChecker *generatorChecker = nullptr;
  AliasInstantiator *aliasInstantiator = nullptr;
  ClassValueChecker *classValueChecker = nullptr;
  BuiltinModuleChecker *builtinModuleChecker = nullptr;
  GenericClassInstantiator *genericClassInstantiator = nullptr;
};

}
