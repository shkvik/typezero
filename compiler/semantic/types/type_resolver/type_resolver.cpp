#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "compiler/program/library/builtin_names/type_name_table/type_name_table.h"
#include "compiler/program/library/library_module_names/library_module_names.h"
#include "compiler/semantic/bodies/classes/class_value_checker/class_value_checker.h"
#include "compiler/semantic/bodies/classes/decorator_checker/decorator_checker.h"
#include "compiler/semantic/bodies/iterators/generator_checker/generator_checker.h"
#include "compiler/semantic/bodies/objects/dict_checker/dict_checker.h"
#include "compiler/semantic/declare/type_declarer/type_declarer.h"
#include "compiler/semantic/flow/predicate_checker/predicate_checker.h"
#include "compiler/semantic/generics/alias_instantiator/alias_instantiator.h"
#include "compiler/semantic/generics/generic_class_instantiator/generic_class_instantiator.h"
#include "compiler/semantic/library/async/async_checker/async_checker.h"
#include "compiler/semantic/library/builtin_modules/builtin_module_checker/builtin_module_checker.h"
#include "compiler/semantic/library/bytes/bytes_checker/bytes_checker.h"
#include "compiler/semantic/library/crypto/crypto_checker/crypto_checker.h"
#include "compiler/semantic/library/decimal/decimal_checker/decimal_checker.h"
#include "compiler/semantic/library/decimal/decimal_checker/decimal_checker_errors.h"
#include "compiler/semantic/library/intl/intl_checker/intl_checker.h"
#include "compiler/semantic/library/result/result_types/result_types.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/conditional_types/conditional_types.h"
#include "compiler/semantic/types/template_types/template_types.h"
#include "compiler/semantic/types/type_operators/type_operators.h"
#include "compiler/semantic/types/utility_types/utility_types.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ErrorHandling.h"
#include <cmath>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;

namespace tz {

TypeResolver::TypeResolver(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->dictChecker = &all.dictChecker;
  this->resultTypes = &all.resultTypes;
  this->asyncChecker = &all.asyncChecker;
  this->typeDeclarer = &all.typeDeclarer;
  this->utilityTypes = &all.utilityTypes;
  this->cryptoChecker = &all.cryptoChecker;
  this->templateTypes = &all.templateTypes;
  this->typeOperators = &all.typeOperators;
  this->conditionalTypes = &all.conditionalTypes;
  this->decoratorChecker = &all.decoratorChecker;
  this->generatorChecker = &all.generatorChecker;
  this->aliasInstantiator = &all.aliasInstantiator;
  this->classValueChecker = &all.classValueChecker;
  this->builtinModuleChecker = &all.builtinModuleChecker;
  this->genericClassInstantiator = &all.genericClassInstantiator;
}

const Type *TypeResolver::literalType(const TypeRef &ref) {
  if (TypeNameTable::isName(ref.name, TypeName::STRING)) {
    return Type::stringLiteral(ref.text());
  }
  if (TypeNameTable::isName(ref.name, TypeName::BOOLEAN)) {
    return Type::boolLiteral(ref.number() != 0);
  }
  bool isWhole = ref.number() == std::trunc(ref.number()) &&
                 std::fabs(ref.number()) <= 9007199254740992.0 &&
                 !(ref.number() == 0 && std::signbit(ref.number()));
  const Type *kind = isWhole ? Type::intTy() : Type::doubleTy();

  return Type::numberLiteral(ref.number(), kind);
}

bool TypeResolver::isMethodAt(const TypeRef &ref, size_t index) {
  return index < ref.methods().size() && ref.methods()[index];
}

bool TypeResolver::overloaded(const TypeRef &ref, size_t index) {
  if (!TypeResolver::isMethodAt(ref, index)) {
    return false;
  }
  bool isSeveral = false;
  for (size_t other = 0; other < ref.args.size(); ++other) {
    if (other == index || ref.names()[other] != ref.names()[index]) {
      continue;
    }
    if (!TypeResolver::isMethodAt(ref, other)) {
      return false;
    }
    isSeveral = true;
  }

  return isSeveral;
}

TypeResolver::ResultStandaloneUsable TypeResolver::standalone(const Type *type, bool allowVoid) {
  if (type->isVoid() && !allowVoid) {
    std::string message = ErrorTextTypeUse::voidType();

    return err(TypeResolverStandaloneError{
        .code = TypeResolverStandaloneErrorCode::VOID_TYPE,
        .message = message,
    });
  }
  if (type->isUndefined()) {
    std::string message = ErrorTextTypeUse::undefined();

    return err(TypeResolverStandaloneError{
        .code = TypeResolverStandaloneErrorCode::UNDEFINED_ALONE,
        .message = message,
    });
  }

  return ok(type);
}

TypeResolver::ResultEnumMemberExists TypeResolver::enumMember(const EnumDecl *declaredEnum,
                                                              llvm::StringRef owner,
                                                              llvm::StringRef member) {
  const EnumMember *found = declaredEnum->find(member);
  if (!found) {
    std::string message = ErrorTextTypeName::enumMember(owner, member);

    return err(TypeResolverEnumMemberError{
        .code = TypeResolverEnumMemberErrorCode::NO_SUCH_MEMBER,
        .message = message,
    });
  }

  return ok(found);
}

TypeResolver::ResultArrayBufferGiven TypeResolver::bufferArgument(const TypeRef &ref,
                                                                  const Type *bytes) {
  bool isSingle = ref.args.size() == 1 && ref.args[0].kind() == TypeRefKind::NAME_REF;
  bool isBuffer =
      isSingle && (TypeNameTable::isName(ref.args[0].name, TypeName::ARRAY_BUFFER) ||
                   TypeNameTable::isName(ref.args[0].name, TypeName::ARRAY_BUFFER_LIKE));
  if (!isBuffer) {
    std::string message = ErrorTextGenericRef::bufferArg(ref.name);

    return err(TypeResolverBufferArgumentError{
        .code = TypeResolverBufferArgumentErrorCode::NOT_ARRAY_BUFFER,
        .message = message,
    });
  }

  return ok(bytes);
}

TypeResolver::ResultElementTypeGiven TypeResolver::arrayArgument(const TypeRef &ref) {
  if (ref.args.size() != 1) {
    std::string message = ErrorTextGenericRef::oneArg(ref.name);

    return err(TypeResolverArrayArgumentError{
        .code = TypeResolverArrayArgumentErrorCode::NOT_ONE_ARGUMENT,
        .message = message,
    });
  }

  return ok(&ref.args[0]);
}

TypeResolver::ResultGenericClassUsed TypeResolver::classGeneric(const ClassDecl *cls,
                                                                llvm::StringRef name) {
  if (!cls->isGeneric()) {
    std::string message = ErrorTextGenericRef::plainClass(name);

    return err(TypeResolverClassGenericError{
        .code = TypeResolverClassGenericErrorCode::NOT_GENERIC,
        .message = message,
    });
  }

  return ok(cls);
}

TypeResolver::ResultGenericAliasUsed TypeResolver::aliasGeneric(const TypeAliasDecl *alias,
                                                                llvm::StringRef name) {
  if (alias->typeParams.empty()) {
    std::string message = ErrorTextGenericRef::plainAlias(name);

    return err(TypeResolverAliasGenericError{
        .code = TypeResolverAliasGenericErrorCode::NOT_GENERIC,
        .message = message,
    });
  }

  return ok(alias);
}

TypeResolver::ResultTwoTypeArguments TypeResolver::descriptorArity(const TypeRef &ref) {
  if (ref.args.size() != 2) {
    std::string message = ErrorTextGenericRef::descriptor();

    return err(TypeResolverDescriptorArityError{
        .code = TypeResolverDescriptorArityErrorCode::NOT_TWO_ARGUMENTS,
        .message = message,
    });
  }

  return ok(&ref);
}

TypeResolver::ResultParamTypesAsList TypeResolver::descriptorParams(const Type *params) {
  if (!params->isTuple() && !params->isListParam()) {
    std::string message = ErrorTextGenericRef::paramList();

    return err(TypeResolverDescriptorParamsError{
        .code = TypeResolverDescriptorParamsErrorCode::NOT_A_LIST,
        .message = message,
    });
  }

  return ok(params);
}

TypeResolver::ResultInstanceOfAClass TypeResolver::classInstance(const Type *cls) {
  const Type *made = cls->isClassRef() ? ClassValueChecker::constructedBy(cls) : nullptr;
  if (!made) {
    std::string message = ErrorTextGenericRef::instanceOf(cls->name());

    return err(TypeResolverClassInstanceError{
        .code = TypeResolverClassInstanceErrorCode::NOT_A_CLASS,
        .message = message,
    });
  }

  return ok(made);
}

TypeResolver::ResultArrayElementType TypeResolver::arrayElement(const Type *element) {
  if (element->isVoid() || element->isUndefined()) {
    std::string message = ErrorTextTypeShape::arrayOf(element->name());

    return err(TypeResolverArrayElementError{
        .code = TypeResolverArrayElementErrorCode::NOT_SUPPORTED,
        .message = message,
    });
  }

  return ok(element);
}

TypeResolver::ResultNonVoidUnionPart TypeResolver::unionPart(const Type *type) {
  if (type->isVoid()) {
    std::string message = ErrorTextTypeShape::voidUnion();

    return err(TypeResolverUnionPartError{
        .code = TypeResolverUnionPartErrorCode::VOID_MEMBER,
        .message = message,
    });
  }

  return ok(type);
}

TypeResolver::ResultUnionIsSupported TypeResolver::unionAllowed(const Type *type, bool withNull) {
  if (withNull && !type->nonOptional()->isJson()) {
    std::string message = ErrorTextTypeShape::nullUnion();

    return err(TypeResolverUnionAllowedError{
        .code = TypeResolverUnionAllowedErrorCode::NULL_MEMBER,
        .message = message,
    });
  }
  std::string problem = UnionSlots::unionProblem(type);
  if (!problem.empty()) {
    std::string message = ErrorTextTypeShape::noUnion(type->name(), problem);

    return err(TypeResolverUnionAllowedError{
        .code = TypeResolverUnionAllowedErrorCode::NOT_SUPPORTED,
        .message = message,
    });
  }

  return ok(type);
}

TypeResolver::ResultRestParamIsArray TypeResolver::restParam(const Type *type) {
  if (!type->isArray() && !type->isListParam() && !type->isTuple() && !type->isError()) {
    std::string message = ErrorTextTypeShape::restArray();

    return err(TypeResolverRestParamError{
        .code = TypeResolverRestParamErrorCode::NOT_AN_ARRAY,
        .message = message,
    });
  }

  return ok(type);
}

TypeResolver::ResultParamOrderIsKept TypeResolver::paramOrder(bool optionalBefore) {
  if (optionalBefore) {
    std::string message = ErrorTextTypeShape::paramOrder();

    return err(TypeResolverParamOrderError{
        .code = TypeResolverParamOrderErrorCode::REQUIRED_AFTER_OPTIONAL,
        .message = message,
    });
  }

  return ok(optionalBefore);
}

TypeResolver::ResultTupleElementType TypeResolver::tupleElement(const Type *element) {
  if (element->isListParam()) {
    std::string message = ErrorTextTypeShape::typeList(element->name());

    return err(TypeResolverTupleElementError{
        .code = TypeResolverTupleElementErrorCode::TYPE_LIST,
        .message = message,
    });
  }

  return ok(element);
}

TypeResolver::ResultTupleSpreadJoins
TypeResolver::spreadJoined(llvm::ArrayRef<const Type *> elements, const std::vector<bool> &spread) {
  std::string why;
  const Type *list = ArgumentLists::spreadTuple(elements, spread, why);
  if (!list) {
    return err(TypeResolverSpreadJoinedError{
        .code = TypeResolverSpreadJoinedErrorCode::NOT_JOINED,
        .message = why,
    });
  }

  return ok(list);
}

TypeResolver::ResultOverloadsAllowed
TypeResolver::overloadAllowed(const std::vector<Overload> *overloads, llvm::StringRef name) {
  if (!overloads) {
    std::string message = ErrorTextTypeFields::signatures(name);

    return err(TypeResolverOverloadAllowedError{
        .code = TypeResolverOverloadAllowedErrorCode::NOT_AN_INTERFACE,
        .message = message,
    });
  }

  return ok(name);
}

TypeResolver::ResultUniqueSignatures
TypeResolver::newSignature(const Overload &overload, const Type *type, llvm::StringRef name) {
  if (llvm::is_contained(overload.signatures, type)) {
    std::string message = ErrorTextTypeFields::duplicate(type->name(), name);

    return err(TypeResolverNewSignatureError{
        .code = TypeResolverNewSignatureErrorCode::DUPLICATE_SIGNATURE,
        .message = message,
    });
  }

  return ok(type);
}

TypeResolver::ResultUniqueFieldNames TypeResolver::newField(llvm::ArrayRef<Field> fields,
                                                            llvm::StringRef name) {
  bool isTaken = llvm::any_of(fields, [&](const Field &field) { return field.name == name; });
  if (isTaken) {
    std::string message = ErrorTextTypeFields::sameField(name);

    return err(TypeResolverNewFieldError{
        .code = TypeResolverNewFieldErrorCode::DUPLICATE_FIELD,
        .message = message,
    });
  }

  return ok(name);
}

bool TypeResolver::boundParam(const TypeRef &ref) const {
  if (ref.kind() != TypeRefKind::NAME_REF) {
    return false;
  }
  auto *bound = dyn_cast_or_null<TypeAliasDecl>(this->typeDeclarer->lookupType(ref.name));

  return bound && bound->boundParam;
}

const Type *TypeResolver::resolveType(const TypeRef &ref, bool allowVoid) {
  const Type *type = this->resolveTypeInner(ref);
  if (type->isVoid() && !allowVoid && this->boundParam(ref)) {
    return Type::undefinedTy();
  }
  ResultStandaloneUsable usable = TypeResolver::standalone(type, allowVoid);
  if (usable.isErr()) {
    this->state.getDiagnostics().error(ref.loc, usable.error().message);
    return Type::error();
  }

  return type;
}

const Type *TypeResolver::resolveTypeInner(const TypeRef &ref) {
  switch (ref.kind()) {
    case TypeRefKind::NAME_REF:
      return this->resolveName(ref);
    case TypeRefKind::GENERIC_REF:
      return this->resolveGeneric(ref);
    case TypeRefKind::ARRAY_REF:
      return this->resolveArray(ref);
    case TypeRefKind::UNION_REF:
      return this->resolveUnion(ref);
    case TypeRefKind::LITERAL_REF:
      return TypeResolver::literalType(ref);
    case TypeRefKind::PREDICATE_REF: {
      std::string message = ErrorTextTypeShape::predicate(ref.name);
      this->state.getDiagnostics().error(ref.loc, message);
      return Type::error();
    }
    case TypeRefKind::KEY_OF_REF:
    case TypeRefKind::INDEXED_REF:
    case TypeRefKind::TYPE_OF_REF:
    case TypeRefKind::MAPPED_REF:
      return this->resolveOperator(ref);
    case TypeRefKind::CONDITIONAL_REF:
      return this->conditionalTypes->resolveConditional(ref);
    case TypeRefKind::TEMPLATE_REF:
      return this->templateTypes->resolveTemplate(ref);
    case TypeRefKind::INTERSECTION_REF:
      return this->resolveIntersection(ref);
    case TypeRefKind::INFER_REF:
      return this->resolveInfer(ref);
    case TypeRefKind::FUNCTION_REF:
      return this->resolveFunctionType(ref);
    case TypeRefKind::OBJECT_REF: {
      std::vector<Field> fields = this->resolveFields(ref);
      return Type::object(fields);
    }
    case TypeRefKind::TUPLE_REF:
      return this->resolveTuple(ref);
  }
  llvm_unreachable("unknown type reference");
}

const Type *TypeResolver::instantiate(ClassDecl *cls, std::vector<const Type *> args,
                                      llvm::SMLoc loc) {
  GenericClassInstantiator::ResultClassMade made =
      this->genericClassInstantiator->instantiateClass(cls, std::move(args), loc);
  if (made.isOk()) {
    return made.value()->type;
  }
  GenericClassInstantiatorInstantiateClassErrorCode code = made.error().code;
  if (code != GenericClassInstantiatorInstantiateClassErrorCode::ALREADY_REPORTED) {
    bool isEndless = code == GenericClassInstantiatorInstantiateClassErrorCode::ENDLESS_INSTANCES;
    llvm::SMLoc at = isEndless ? cls->loc() : loc;
    this->state.getDiagnostics().error(at, made.error().message);
  }

  return Type::error();
}

const Type *TypeResolver::resolveDeclared(const TypeRef &ref, Stmt *decl) {
  if (auto *alias = dyn_cast<TypeAliasDecl>(decl)) {
    if (alias->typeParams.empty()) {
      return this->typeDeclarer->resolveAlias(alias);
    }
    return this->aliasInstantiator->instantiateAlias(alias, {}, ref.loc);
  }
  if (auto *cls = dyn_cast<ClassDecl>(decl)) {
    if (cls->isGeneric()) {
      return this->instantiate(cls, {}, ref.loc);
    }
    return ClassValueChecker::finalClass(cls)->type;
  }

  return this->typeDeclarer->enumType(cast<EnumDecl>(decl));
}

TypeResolver::ResultIntlTypeResolved TypeResolver::intlType(llvm::StringRef name) const {
  auto *locale = name == "Intl.Locale" ? dyn_cast_or_null<ClassDecl>(this->typeDeclarer->lookupType(
                                             LibraryModuleNames::kIntlLocaleName))
                                       : nullptr;
  if (locale) {
    return ok(ClassValueChecker::finalClass(locale)->type);
  }
  const Type *intl = IntlChecker::intlTypeNamed(name.drop_front(5));
  if (!intl) {
    std::string message = ErrorTextTypeName::notFound(name);

    return err(TypeResolverIntlTypeError{
        .code = TypeResolverIntlTypeErrorCode::NOT_FOUND,
        .message = message,
    });
  }

  return ok(intl);
}

const Type *TypeResolver::builtinNamed(llvm::StringRef name) const {
  if (TypeNameTable::isName(name, TypeName::TEMPLATE_STRINGS_ARRAY)) {
    const Type *text = Type::string();
    return Type::array(text);
  }
  if (TypeNameTable::isName(name, TypeName::REG_EXP)) {
    return Type::regExp();
  }
  if (TypeNameTable::isName(name, TypeName::DATE)) {
    return Type::date();
  }
  if (TypeNameTable::isName(name, TypeName::TIMEOUT)) {
    return Type::timeout();
  }
  if (TypeNameTable::isName(name, TypeName::IMMEDIATE)) {
    return Type::immediate();
  }
  if (const Type *bytes = BytesChecker::bytesTypeNamed(name)) {
    return bytes;
  }
  const NodeName *node = this->builtinModuleChecker->nodeName(name);
  bool isReadline =
      node &&
      (node->module == NodeModule::Readline || node->module == NodeModule::ReadlinePromises) &&
      node->name == "Interface";
  if (isReadline) {
    Type::Runtime runtime = node->module == NodeModule::Readline ? Type::Runtime::Readline
                                                                 : Type::Runtime::ReadlinePromises;
    return Type::runtimeObject(runtime);
  }
  if (const Type *crypto = this->cryptoChecker->cryptoTypeNamed(name)) {
    return crypto;
  }
  if (TypeNameTable::isName(name, TypeName::REG_EXP_MATCH_ARRAY)) {
    return Type::match(-1, {}, nullptr, false);
  }

  return nullptr;
}

const Type *TypeResolver::resolveDict(const TypeRef &ref) {
  DictChecker::ResultDictTypeResolved dict = this->dictChecker->resolveDictType(ref);
  if (dict.isOk()) {
    return dict.value();
  }
  if (dict.error().code == DictCheckerResolveDictTypeErrorCode::NOT_A_DICT) {
    return nullptr;
  }
  bool isOnKey = DictChecker::keyArgument(dict.error().code);
  llvm::SMLoc at = isOnKey ? ref.args[0].loc : ref.loc;
  this->state.getDiagnostics().error(at, dict.error().message);

  return Type::error();
}

std::string TypeResolver::missingText(llvm::StringRef name) const {
  if (TypeNameTable::isName(name, TypeName::ARRAY)) {
    return ErrorTextTypeName::arrayArg();
  }
  std::optional<TypeName> result = this->resultTypes->findResultType(name);
  if (result.has_value()) {
    return ErrorTextTypeName::resultArgs(name);
  }
  bool isGenerator = TypeNameTable::isName(name, TypeName::GENERATOR) ||
                     TypeNameTable::isName(name, TypeName::ITERABLE_ITERATOR);
  if (isGenerator) {
    return ErrorTextTypeName::valuesArg(name);
  }
  if (TypeNameTable::isName(name, TypeName::PROMISE)) {
    return ErrorTextTypeName::promiseArg();
  }
  if (TypeNameTable::isName(name, TypeName::NULL_TYPE)) {
    return ErrorTextTypeName::nullType();
  }
  if (TypeNameTable::isName(name, TypeName::LONG)) {
    return ErrorTextTypeName::longType();
  }
  if (TypeNameTable::isName(name, TypeName::SHARED_ARRAY_BUFFER)) {
    return ErrorTextTypeName::shared();
  }

  return ErrorTextTypeName::notFound(name);
}

const Type *TypeResolver::missingType(const TypeRef &ref) {
  if (TypeNameTable::isName(ref.name, TypeName::BIGINT)) {
    std::string what = ErrorTextTypeName::bigintType();
    std::string bigint = ErrorTextBigInt::none(what);
    this->state.getDiagnostics().error(ref.loc, bigint);
    return Type::error();
  }
  bool isBigintArray = TypeNameTable::isName(ref.name, TypeName::BIG_INT64_ARRAY) ||
                       TypeNameTable::isName(ref.name, TypeName::BIG_UINT64_ARRAY);
  if (isBigintArray) {
    std::string what = ErrorTextTypeName::bigintName(ref.name);
    std::string bigint = ErrorTextBigInt::none(what);
    this->state.getDiagnostics().error(ref.loc, bigint);
    return Type::error();
  }
  std::string message = this->missingText(ref.name);
  this->state.getDiagnostics().error(ref.loc, message);

  return Type::error();
}

const Type *TypeResolver::resolveName(const TypeRef &ref) {
  if (ref.name.empty()) {
    return Type::error();
  }
  if (const Type *type = Type::fromName(ref.name)) {
    return type;
  }
  if (TypeNameTable::isName(ref.name, TypeName::NEVER) &&
      !this->typeDeclarer->lookupType(ref.name)) {
    return Type::never();
  }
  if (Stmt *decl = this->typeDeclarer->lookupType(ref.name)) {
    return this->resolveDeclared(ref, decl);
  }
  auto [owner, member] = ref.name.split('.');
  auto *declaredEnum =
      member.empty() ? nullptr : dyn_cast_or_null<EnumDecl>(this->typeDeclarer->lookupType(owner));
  if (declaredEnum) {
    this->typeDeclarer->resolveEnum(declaredEnum);
    ResultEnumMemberExists found = TypeResolver::enumMember(declaredEnum, owner, member);
    if (found.isErr()) {
      this->state.getDiagnostics().error(ref.loc, found.error().message);
      return Type::error();
    }
    const EnumMember *value = found.value();
    if (value->isString) {
      return Type::stringLiteral(value->text);
    }
    return Type::numberLiteral(double(value->number), Type::intTy());
  }
  if (ref.name.starts_with("Intl.")) {
    ResultIntlTypeResolved intl = this->intlType(ref.name);
    if (intl.isErr()) {
      this->state.getDiagnostics().error(ref.loc, intl.error().message);
      return Type::error();
    }
    return intl.value();
  }
  if (const Type *builtin = this->builtinNamed(ref.name)) {
    return builtin;
  }
  if (const Type *dict = this->resolveDict(ref)) {
    return dict;
  }
  if (TypeNameTable::isName(ref.name, TypeName::CONSTRUCTOR)) {
    const Type *object = Type::anyObject();
    return Type::constructorOf(object);
  }
  if (TypeNameTable::isName(ref.name, TypeName::CONSTRUCTOR_ARGS)) {
    ClassValueChecker::ResultConstructorArgs args = this->classValueChecker->constructorArgs();
    if (args.isErr()) {
      this->state.getDiagnostics().error(ref.loc, args.error().message);
      return Type::error();
    }
    return args.value();
  }

  return this->missingType(ref);
}

const Type *TypeResolver::resolveArrayGeneric(const TypeRef &ref) {
  ResultElementTypeGiven given = TypeResolver::arrayArgument(ref);
  if (given.isErr()) {
    this->state.getDiagnostics().error(ref.loc, given.error().message);
    return Type::error();
  }
  TypeRef array;
  array.form = ArrayTypeRef{};
  array.loc = ref.loc;
  array.args = ref.args;

  return this->resolveTypeInner(array);
}

const Type *TypeResolver::resolveGenericClass(const TypeRef &ref, ClassDecl *cls) {
  std::vector<const Type *> args;
  for (const TypeRef &arg : ref.args) {
    const Type *type = this->resolveType(arg, true);
    if (type->isError()) {
      return type;
    }
    args.push_back(type);
  }
  ResultGenericClassUsed generic = TypeResolver::classGeneric(cls, ref.name);
  if (generic.isErr()) {
    this->state.getDiagnostics().error(ref.loc, generic.error().message);
    return Type::error();
  }

  return this->instantiate(cls, std::move(args), ref.loc);
}

const Type *TypeResolver::resolveGenericAlias(const TypeRef &ref, TypeAliasDecl *alias) {
  std::vector<const Type *> args;
  for (const TypeRef &arg : ref.args) {
    const Type *type = this->conditionalTypes->resolveTypeArgument(arg);
    if (type->isError()) {
      return type;
    }
    args.push_back(type);
  }
  ResultGenericAliasUsed generic = TypeResolver::aliasGeneric(alias, ref.name);
  if (generic.isErr()) {
    this->state.getDiagnostics().error(ref.loc, generic.error().message);
    return Type::error();
  }

  return this->aliasInstantiator->instantiateAlias(alias, std::move(args), ref.loc);
}

const Type *TypeResolver::resolveIterator(const TypeRef &ref) {
  GeneratorChecker::ResultResolvedIterator iterator =
      this->generatorChecker->resolveIteratorType(ref);
  if (iterator.isOk()) {
    return iterator.value();
  }
  if (iterator.error().code == GeneratorCheckerResolveIteratorTypeErrorCode::NOT_AN_ITERATOR) {
    return nullptr;
  }
  this->state.getDiagnostics().error(ref.loc, iterator.error().message);

  return Type::error();
}

const Type *TypeResolver::resolvePromise(const TypeRef &ref) {
  AsyncChecker::ResultPromiseTypeRef promise = this->asyncChecker->resolvePromiseType(ref);
  if (promise.isOk()) {
    return promise.value();
  }
  if (promise.error().code == AsyncCheckerResolvePromiseTypeErrorCode::NOT_A_PROMISE) {
    return nullptr;
  }
  this->state.getDiagnostics().error(ref.loc, promise.error().message);

  return Type::error();
}

const Type *TypeResolver::resolveUtility(const TypeRef &ref) {
  UtilityTypes::ResultUtilityApplied utility = this->utilityTypes->resolveUtilityType(ref);
  if (utility.isOk()) {
    return utility.value();
  }
  if (utility.error().code == UtilityTypesResolveUtilityTypeErrorCode::NOT_A_UTILITY) {
    return nullptr;
  }
  this->state.getDiagnostics().error(ref.loc, utility.error().message);

  return Type::error();
}

const Type *TypeResolver::resolveDescriptor(const TypeRef &ref) {
  ResultTwoTypeArguments arity = TypeResolver::descriptorArity(ref);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(ref.loc, arity.error().message);
    return Type::error();
  }
  const Type *params = this->resolveType(ref.args[0], false);
  const Type *returned = this->resolveType(ref.args[1], true);
  if (params->isError() || returned->isError()) {
    return Type::error();
  }
  ResultParamTypesAsList list = TypeResolver::descriptorParams(params);
  if (list.isErr()) {
    this->state.getDiagnostics().error(ref.args[0].loc, list.error().message);
    return Type::error();
  }

  return this->decoratorChecker->methodDescriptorType(params, returned);
}

const Type *TypeResolver::resolveConstructor(const TypeRef &ref) {
  ClassValueChecker::ResultConstructorType constructor =
      this->classValueChecker->resolveConstructorType(ref);
  if (constructor.isOk()) {
    return constructor.value();
  }
  bool isObjectType = constructor.error().code ==
                      ClassValueCheckerResolveConstructorTypeErrorCode::NOT_AN_OBJECT_TYPE;
  llvm::SMLoc at = isObjectType ? ref.args[0].loc : ref.loc;
  this->state.getDiagnostics().error(at, constructor.error().message);

  return Type::error();
}

const Type *TypeResolver::resolveInstanceType(const TypeRef &ref) {
  const Type *cls = this->resolveType(ref.args[0], false);
  if (cls->isError()) {
    return cls;
  }
  ResultInstanceOfAClass made = TypeResolver::classInstance(cls);
  if (made.isErr()) {
    this->state.getDiagnostics().error(ref.args[0].loc, made.error().message);
    return Type::error();
  }

  return made.value();
}

const Type *TypeResolver::resolveGeneric(const TypeRef &ref) {
  const Type *bytes = BytesChecker::bytesTypeNamed(ref.name);
  if (bytes && bytes->isTypedArray() && !this->typeDeclarer->lookupType(ref.name)) {
    ResultArrayBufferGiven buffer = TypeResolver::bufferArgument(ref, bytes);
    if (buffer.isErr()) {
      this->state.getDiagnostics().error(ref.loc, buffer.error().message);
      return Type::error();
    }
    return buffer.value();
  }
  std::optional<TypeName> result = this->resultTypes->findResultType(ref.name);
  if (result.has_value()) {
    return this->resultTypes->resolveResultType(ref, *result);
  }
  const TypeNameInfo *builtin = TypeNameTable::find(ref.name);
  bool isArray = builtin != nullptr && builtin->form.is<ArrayTypeName>();
  if (isArray) {
    return this->resolveArrayGeneric(ref);
  }
  if (auto *cls = dyn_cast_or_null<ClassDecl>(this->typeDeclarer->lookupType(ref.name))) {
    return this->resolveGenericClass(ref, cls);
  }
  if (auto *alias = dyn_cast_or_null<TypeAliasDecl>(this->typeDeclarer->lookupType(ref.name))) {
    return this->resolveGenericAlias(ref, alias);
  }
  if (const Type *iterator = this->resolveIterator(ref)) {
    return iterator;
  }
  if (const Type *dict = this->resolveDict(ref)) {
    return dict;
  }
  if (const Type *promise = this->resolvePromise(ref)) {
    return promise;
  }
  if (TypeNameTable::isName(ref.name, TypeName::METHOD_DESCRIPTOR)) {
    return this->resolveDescriptor(ref);
  }
  if (const Type *utility = this->resolveUtility(ref)) {
    return utility;
  }
  if (TypeNameTable::isName(ref.name, TypeName::CONSTRUCTOR)) {
    return this->resolveConstructor(ref);
  }
  if (TypeNameTable::isName(ref.name, TypeName::INSTANCE_TYPE) && ref.args.size() == 1) {
    return this->resolveInstanceType(ref);
  }
  std::string message = ErrorTextGenericRef::noSupport(ref.name);
  this->state.getDiagnostics().error(ref.loc, message);

  return Type::error();
}

const Type *TypeResolver::resolveArray(const TypeRef &ref) {
  const Type *element = this->resolveTypeInner(ref.args[0]);
  ResultArrayElementType allowed = TypeResolver::arrayElement(element);
  if (allowed.isErr()) {
    this->state.getDiagnostics().error(ref.loc, allowed.error().message);
    return Type::error();
  }

  return Type::array(element);
}

const Type *TypeResolver::resolveUnion(const TypeRef &ref) {
  bool isWithNull = false;
  std::vector<const Type *> members;
  for (const TypeRef &member : ref.args) {
    if (member.kind() == TypeRefKind::NAME_REF &&
        TypeNameTable::isName(member.name, TypeName::NULL_TYPE)) {
      isWithNull = true;
      continue;
    }
    const Type *type = this->resolveTypeInner(member);
    if (type->isError()) {
      return type;
    }
    ResultNonVoidUnionPart part = TypeResolver::unionPart(type);
    if (part.isErr()) {
      this->state.getDiagnostics().error(ref.loc, part.error().message);
      return Type::error();
    }
    members.push_back(type);
  }
  const Type *type = Type::unionOf(members);
  ResultUnionIsSupported supported = TypeResolver::unionAllowed(type, isWithNull);
  if (supported.isErr()) {
    this->state.getDiagnostics().error(ref.loc, supported.error().message);
    return Type::error();
  }

  return type;
}

const Type *TypeResolver::resolveOperator(const TypeRef &ref) {
  TypeOperators::ResultTypeOpResolved resolved = this->typeOperators->resolveTypeOp(ref);
  if (resolved.isOk()) {
    return resolved.value();
  }
  llvm::SMLoc at = ref.loc;
  switch (resolved.error().code) {
    case TypeOperatorsResolveTypeOpErrorCode::RENAME_NOT_STRING:
      at = ref.args[2].loc;
      break;
    case TypeOperatorsResolveTypeOpErrorCode::KEYS_NOT_STRINGS:
      at = ref.args[0].loc;
      break;
    case TypeOperatorsResolveTypeOpErrorCode::NOT_AN_OBJECT_TYPE:
    case TypeOperatorsResolveTypeOpErrorCode::NO_SUCH_MEMBER:
    case TypeOperatorsResolveTypeOpErrorCode::TYPE_OF_UNKNOWN:
    case TypeOperatorsResolveTypeOpErrorCode::NO_SUCH_FIELD:
      break;
  }
  this->state.getDiagnostics().error(at, resolved.error().message);

  return Type::error();
}

const Type *TypeResolver::resolveIntersection(const TypeRef &ref) {
  ConditionalTypes::ResultIntersectionJoined joined =
      this->conditionalTypes->resolveIntersection(ref);
  if (joined.isErr()) {
    this->state.getDiagnostics().error(ref.loc, joined.error().message);
    return Type::error();
  }

  return joined.value();
}

TypeResolver::ResultInferTypeBinding TypeResolver::inferBinding(const TypeRef &ref) const {
  auto *binding = dyn_cast_or_null<TypeAliasDecl>(this->typeDeclarer->lookupType(ref.name));
  if (!this->state.getTypeContext().isInsideExtendsClause() || !binding || !binding->resolved) {
    std::string message = ErrorTextTypeShape::inferOnly();

    return err(TypeResolverInferBindingError{
        .code = TypeResolverInferBindingErrorCode::OUTSIDE_EXTENDS,
        .message = message,
    });
  }

  return ok(binding->resolved);
}

const Type *TypeResolver::resolveInfer(const TypeRef &ref) {
  ResultInferTypeBinding binding = this->inferBinding(ref);
  if (binding.isErr()) {
    this->state.getDiagnostics().error(ref.loc, binding.error().message);
    return Type::error();
  }

  return binding.value();
}

const Type *TypeResolver::resolveFunctionType(const TypeRef &ref) {
  std::vector<const Type *> params;
  size_t minArgs = 0;
  bool isOptional = false;
  bool isValid = true;
  size_t count = ref.args.size() - 1;
  bool hasThis = count > 0 && !ref.names().empty() && ref.names()[0] == "this" &&
                 !ref.optional()[0] && !(ref.rest() && count == 1);
  for (size_t index = 0; index < count; ++index) {
    bool hasRest = ref.rest() && index + 1 == count;
    this->state.getTypeContext().enterListContext(hasRest);
    const Type *type = this->resolveType(ref.args[index], false);
    this->state.getTypeContext().leaveListContext();
    isValid &= !type->isError();
    if (hasRest) {
      ResultRestParamIsArray list = TypeResolver::restParam(type);
      if (list.isErr()) {
        this->state.getDiagnostics().error(ref.args[index].loc, list.error().message);
        isValid = false;
      }
    } else if (ref.optional()[index]) {
      type = Type::optional(type);
      isOptional = true;
    } else {
      ResultParamOrderIsKept order = TypeResolver::paramOrder(isOptional);
      if (order.isErr()) {
        this->state.getDiagnostics().error(ref.args[index].loc, order.error().message);
        isValid = false;
      }
      minArgs = index + 1;
    }
    params.push_back(type);
  }
  const TypeRef &returned = ref.args.back();
  bool isPredicate = returned.kind() == TypeRefKind::PREDICATE_REF;
  const Type *returnType = isPredicate ? Type::boolTy() : this->resolveType(returned, true);
  if (isPredicate && returned.asserts()) {
    const Type *type = returned.args.empty() ? nullptr : this->resolveType(returned.args[0], false);
    std::string shown = returned.name.str() + (type ? " is " + type->name().str() : "");
    std::string message = ErrorTextAsserts::notAllowed(shown, type != nullptr, "");
    this->state.getDiagnostics().error(returned.loc, message);
    returnType = Type::error();
  }
  if (!isValid || returnType->isError()) {
    return Type::error();
  }

  return Type::function(params, minArgs, ref.rest(), returnType, hasThis);
}

TypeResolver::ResultTupleLengthFixed TypeResolver::fixedLength(const Type *list) const {
  if (list->isTuple() && list->tupleRest() && !this->state.getTypeContext().isInsideListContext() &&
      !this->state.getTypeContext().isInsideExtendsClause()) {
    std::string message = ErrorTextTypeShape::tupleRest();

    return err(TypeResolverFixedLengthError{
        .code = TypeResolverFixedLengthErrorCode::REST_ELEMENT,
        .message = message,
    });
  }

  return ok(list);
}

const Type *TypeResolver::resolveSpreadTuple(const TypeRef &ref,
                                             const std::vector<const Type *> &elements) {
  bool isWaiting = false;
  for (size_t index = 0; index < elements.size(); ++index) {
    isWaiting |= ref.spread()[index] && elements[index]->typeOp() != TypeOp::None &&
                 !elements[index]->isListParam();
  }
  bool isKnown = llvm::none_of(elements, [](const Type *type) { return type->isError(); });
  if (isWaiting && isKnown) {
    std::vector<const Type *> operands;
    for (size_t index = 0; index < elements.size(); ++index) {
      operands.push_back(Type::boolLiteral(ref.spread()[index]));
      operands.push_back(elements[index]);
    }
    const Type *spread = Type::typeOp(TypeOp::Spread, operands);
    return this->typeOperators->evalTypeOp(spread, operands);
  }
  ResultTupleSpreadJoins joined = TypeResolver::spreadJoined(elements, ref.spread());
  if (joined.isErr()) {
    this->state.getDiagnostics().error(ref.loc, joined.error().message);
    return Type::error();
  }
  const Type *list = joined.value();
  ResultTupleLengthFixed fixed = this->fixedLength(list);
  if (fixed.isErr()) {
    this->state.getDiagnostics().error(ref.loc, fixed.error().message);
    return Type::error();
  }

  return list;
}

const Type *TypeResolver::resolveTuple(const TypeRef &ref) {
  std::vector<const Type *> elements;
  for (const TypeRef &element : ref.args) {
    elements.push_back(this->resolveType(element, false));
  }
  if (llvm::is_contained(ref.spread(), true)) {
    return this->resolveSpreadTuple(ref, elements);
  }
  for (size_t index = 0; index < elements.size(); ++index) {
    ResultTupleElementType element = TypeResolver::tupleElement(elements[index]);
    if (element.isErr()) {
      this->state.getDiagnostics().error(ref.args[index].loc, element.error().message);
      return Type::error();
    }
  }
  bool isReadonly = !ref.readonly().empty() && ref.readonly()[0];

  return Type::tuple(elements, isReadonly);
}

std::vector<Field> TypeResolver::resolveFields(const TypeRef &ref,
                                               std::vector<Overload> *overloads) {
  std::vector<Field> fields;
  for (size_t index = 0; index < ref.args.size(); ++index) {
    llvm::StringRef name = ref.names()[index];
    if (TypeResolver::overloaded(ref, index)) {
      ResultOverloadsAllowed allowed = TypeResolver::overloadAllowed(overloads, name);
      if (allowed.isErr()) {
        this->state.getDiagnostics().error(ref.locs()[index], allowed.error().message);
        continue;
      }
      const Type *type = this->resolveType(ref.args[index], false);
      auto found = llvm::find_if(*overloads,
                                 [&](const Overload &overload) { return overload.name == name; });
      if (found == overloads->end()) {
        found = overloads->insert(overloads->end(), Overload{name.str(), {}});
      }
      ResultUniqueSignatures unique = TypeResolver::newSignature(*found, type, name);
      if (unique.isErr()) {
        this->state.getDiagnostics().error(ref.locs()[index], unique.error().message);
      } else if (!type->isError()) {
        found->signatures.push_back(type);
      }
      continue;
    }
    ResultUniqueFieldNames unique = TypeResolver::newField(fields, name);
    if (unique.isErr()) {
      this->state.getDiagnostics().error(ref.locs()[index], unique.error().message);
      continue;
    }
    const Type *type = this->resolveType(ref.args[index], false);
    if (ref.optional()[index]) {
      type = Type::optional(type);
    }
    fields.push_back({name.str(), type, ref.readonly()[index]});
  }

  return fields;
}

}
