#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "compiler/semantic/types/type_resolver/type_resolver_errors.h"

namespace tz {

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

class TypeResolver {
public:
  TypeResolver(SemanticState &owner, SemanticParts &all);

  const Type *resolveTypeInner(const TypeRef &ref);
  const Type *resolveType(const TypeRef &ref, bool allowVoid);
  std::vector<Field> resolveFields(const TypeRef &ref, std::vector<Overload> *overloads = nullptr);

private:
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

  static const Type *literalType(const TypeRef &ref);
  static bool isMethodAt(const TypeRef &ref, size_t index);
  static bool overloaded(const TypeRef &ref, size_t index);
  static ResultNonVoidUnionPart unionPart(const Type *type);
  static ResultRestParamIsArray restParam(const Type *type);
  static ResultInstanceOfAClass classInstance(const Type *cls);
  static ResultParamOrderIsKept paramOrder(bool optionalBefore);
  static ResultArrayElementType arrayElement(const Type *element);
  static ResultTupleElementType tupleElement(const Type *element);
  static ResultElementTypeGiven arrayArgument(const TypeRef &ref);
  static ResultTwoTypeArguments descriptorArity(const TypeRef &ref);
  static ResultParamTypesAsList descriptorParams(const Type *params);
  static ResultStandaloneUsable standalone(const Type *type, bool allowVoid);
  static ResultUnionIsSupported unionAllowed(const Type *type, bool withNull);
  static ResultArrayBufferGiven bufferArgument(const TypeRef &ref, const Type *bytes);
  static ResultGenericClassUsed classGeneric(const ClassDecl *cls, llvm::StringRef name);
  static ResultUniqueFieldNames newField(llvm::ArrayRef<Field> fields, llvm::StringRef name);
  static ResultGenericAliasUsed aliasGeneric(const TypeAliasDecl *alias, llvm::StringRef name);
  static ResultOverloadsAllowed overloadAllowed(const std::vector<Overload> *overloads,
                                                llvm::StringRef name);
  static ResultUniqueSignatures newSignature(const Overload &overload, const Type *type,
                                             llvm::StringRef name);
  static ResultTupleSpreadJoins spreadJoined(llvm::ArrayRef<const Type *> elements,
                                             const std::vector<bool> &spread);
  static ResultEnumMemberExists enumMember(const EnumDecl *declaredEnum, llvm::StringRef owner,
                                           llvm::StringRef member);

  bool boundParam(const TypeRef &ref) const;
  std::string missingText(llvm::StringRef name) const;
  const Type *builtinNamed(llvm::StringRef name) const;
  ResultTupleLengthFixed fixedLength(const Type *list) const;
  ResultIntlTypeResolved intlType(llvm::StringRef name) const;
  ResultInferTypeBinding inferBinding(const TypeRef &ref) const;

  const Type *resolveName(const TypeRef &ref);
  const Type *resolveDict(const TypeRef &ref);
  const Type *missingType(const TypeRef &ref);
  const Type *resolveArray(const TypeRef &ref);
  const Type *resolveUnion(const TypeRef &ref);
  const Type *resolveTuple(const TypeRef &ref);
  const Type *resolveInfer(const TypeRef &ref);
  const Type *resolveGeneric(const TypeRef &ref);
  const Type *resolvePromise(const TypeRef &ref);
  const Type *resolveUtility(const TypeRef &ref);
  const Type *resolveOperator(const TypeRef &ref);
  const Type *resolveIterator(const TypeRef &ref);
  const Type *resolveDescriptor(const TypeRef &ref);
  const Type *resolveConstructor(const TypeRef &ref);
  const Type *resolveArrayGeneric(const TypeRef &ref);
  const Type *resolveInstanceType(const TypeRef &ref);
  const Type *resolveIntersection(const TypeRef &ref);
  const Type *resolveFunctionType(const TypeRef &ref);
  const Type *resolveDeclared(const TypeRef &ref, Stmt *decl);
  const Type *resolveGenericClass(const TypeRef &ref, ClassDecl *cls);
  const Type *resolveGenericAlias(const TypeRef &ref, TypeAliasDecl *alias);
  const Type *instantiate(ClassDecl *cls, std::vector<const Type *> args, llvm::SMLoc loc);
  const Type *resolveSpreadTuple(const TypeRef &ref, const std::vector<const Type *> &elements);

  SemanticState &state;
  DictChecker *dictChecker = nullptr;
  ResultTypes *resultTypes = nullptr;
  AsyncChecker *asyncChecker = nullptr;
  TypeDeclarer *typeDeclarer = nullptr;
  UtilityTypes *utilityTypes = nullptr;
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
