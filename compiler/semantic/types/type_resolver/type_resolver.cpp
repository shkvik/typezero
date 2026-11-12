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

namespace {

constexpr double kMaxWholeNumber = 9007199254740992.0;
constexpr llvm::StringLiteral kIntlPrefix = "Intl.";
constexpr llvm::StringLiteral kIntlLocaleType = "Intl.Locale";
constexpr llvm::StringLiteral kReadlineInterface = "Interface";
constexpr llvm::StringLiteral kThisParam = "this";

}

TypeResolver::TypeResolver(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->dictChecker = &all.dictChecker;
  this->resultTypes = &all.resultTypes;
  this->intlChecker = &all.intlChecker;
  this->asyncChecker = &all.asyncChecker;
  this->typeDeclarer = &all.typeDeclarer;
  this->utilityTypes = &all.utilityTypes;
  this->bytesChecker = &all.bytesChecker;
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

const Type *TypeResolver::literalType(const TypeRef &ref) const {
  if (TypeNameTable::isName(ref.name, TypeName::STRING)) {
    return Type::stringLiteral(ref.text());
  }
  if (TypeNameTable::isName(ref.name, TypeName::BOOLEAN)) {
    return Type::boolLiteral(ref.number() != 0);
  }
  double value = ref.number();
  bool isNegativeZero = value == 0 && std::signbit(value);
  bool isWhole = value == std::trunc(value) && std::fabs(value) <= kMaxWholeNumber;
  if (isWhole && !isNegativeZero) {
    return Type::numberLiteral(value, Type::intTy());
  }

  return Type::numberLiteral(value, Type::doubleTy());
}

bool TypeResolver::isMethodAt(const TypeRef &ref, size_t index) const {
  return index < ref.methods().size() && ref.methods()[index];
}

bool TypeResolver::isOverloaded(const TypeRef &ref, size_t index) const {
  if (!this->isMethodAt(ref, index)) {
    return false;
  }
  size_t sameNamed = 0;
  for (size_t other = 0; other < ref.args.size(); ++other) {
    bool isSameName = other != index && ref.names()[other] == ref.names()[index];
    if (isSameName && !this->isMethodAt(ref, other)) {
      return false;
    }
    if (isSameName) {
      sameNamed += 1;
    }
  }

  return sameNamed > 0;
}

TypeResolver::ResultStandaloneUsable TypeResolver::checkNotVoid(const Type *type) const {
  if (type->isVoid()) {
    auto message = ErrorTextTypeUse::voidType();

    return err(TypeResolverStandaloneError{
        .code = TypeResolverStandaloneErrorCode::VOID_TYPE,
        .message = message,
    });
  }

  return ok(type);
}

TypeResolver::ResultStandaloneUsable TypeResolver::checkNotUndefined(const Type *type) const {
  if (type->isUndefined()) {
    auto message = ErrorTextTypeUse::undefined();

    return err(TypeResolverStandaloneError{
        .code = TypeResolverStandaloneErrorCode::UNDEFINED_ALONE,
        .message = message,
    });
  }

  return ok(type);
}

TypeResolver::ResultEnumMemberExists TypeResolver::enumMember(const EnumDecl *declaredEnum,
                                                              llvm::StringRef owner,
                                                              llvm::StringRef member) const {
  const auto *found = declaredEnum->find(member);
  if (found == nullptr) {
    auto message = ErrorTextTypeName::enumMember(owner, member);

    return err(TypeResolverEnumMemberError{
        .code = TypeResolverEnumMemberErrorCode::NO_SUCH_MEMBER,
        .message = message,
    });
  }

  return ok(found);
}

TypeResolver::ResultArrayBufferGiven TypeResolver::bufferArgument(const TypeRef &ref,
                                                                  const Type *bytes) const {
  bool isSingle = ref.args.size() == 1 && ref.args[0].kind() == TypeRefKind::NAME_REF;
  bool isBuffer =
      isSingle && (TypeNameTable::isName(ref.args[0].name, TypeName::ARRAY_BUFFER) ||
                   TypeNameTable::isName(ref.args[0].name, TypeName::ARRAY_BUFFER_LIKE));
  if (!isBuffer) {
    auto message = ErrorTextGenericRef::bufferArg(ref.name);

    return err(TypeResolverBufferArgumentError{
        .code = TypeResolverBufferArgumentErrorCode::NOT_ARRAY_BUFFER,
        .message = message,
    });
  }

  return ok(bytes);
}

TypeResolver::ResultElementTypeGiven TypeResolver::arrayArgument(const TypeRef &ref) const {
  if (ref.args.size() != 1) {
    auto message = ErrorTextGenericRef::oneArg(ref.name);

    return err(TypeResolverArrayArgumentError{
        .code = TypeResolverArrayArgumentErrorCode::NOT_ONE_ARGUMENT,
        .message = message,
    });
  }

  return ok(&ref.args[0]);
}

TypeResolver::ResultGenericClassUsed TypeResolver::classGeneric(const ClassDecl *classDecl,
                                                                llvm::StringRef name) const {
  if (!classDecl->isGeneric()) {
    auto message = ErrorTextGenericRef::plainClass(name);

    return err(TypeResolverClassGenericError{
        .code = TypeResolverClassGenericErrorCode::NOT_GENERIC,
        .message = message,
    });
  }

  return ok(classDecl);
}

TypeResolver::ResultGenericAliasUsed TypeResolver::aliasGeneric(const TypeAliasDecl *alias,
                                                                llvm::StringRef name) const {
  if (alias->typeParams.empty()) {
    auto message = ErrorTextGenericRef::plainAlias(name);

    return err(TypeResolverAliasGenericError{
        .code = TypeResolverAliasGenericErrorCode::NOT_GENERIC,
        .message = message,
    });
  }

  return ok(alias);
}

TypeResolver::ResultTwoTypeArguments TypeResolver::descriptorArity(const TypeRef &ref) const {
  if (ref.args.size() != 2) {
    auto message = ErrorTextGenericRef::descriptor();

    return err(TypeResolverDescriptorArityError{
        .code = TypeResolverDescriptorArityErrorCode::NOT_TWO_ARGUMENTS,
        .message = message,
    });
  }

  return ok(&ref);
}

TypeResolver::ResultParamTypesAsList TypeResolver::descriptorParams(const Type *params) const {
  if (!params->isTuple() && !params->isListParam()) {
    auto message = ErrorTextGenericRef::paramList();

    return err(TypeResolverDescriptorParamsError{
        .code = TypeResolverDescriptorParamsErrorCode::NOT_A_LIST,
        .message = message,
    });
  }

  return ok(params);
}

TypeResolver::ResultInstanceOfAClass TypeResolver::classInstance(const Type *classType) const {
  const Type *made =
      classType->isClassRef() ? ClassValueChecker::constructedBy(classType) : nullptr;
  if (made == nullptr) {
    auto message = ErrorTextGenericRef::instanceOf(classType->name());

    return err(TypeResolverClassInstanceError{
        .code = TypeResolverClassInstanceErrorCode::NOT_A_CLASS,
        .message = message,
    });
  }

  return ok(made);
}

TypeResolver::ResultArrayElementType TypeResolver::arrayElement(const Type *element) const {
  if (element->isVoid() || element->isUndefined()) {
    auto message = ErrorTextTypeShape::arrayOf(element->name());

    return err(TypeResolverArrayElementError{
        .code = TypeResolverArrayElementErrorCode::NOT_SUPPORTED,
        .message = message,
    });
  }

  return ok(element);
}

TypeResolver::ResultNonVoidUnionPart TypeResolver::unionPart(const Type *type) const {
  if (type->isVoid()) {
    auto message = ErrorTextTypeShape::voidUnion();

    return err(TypeResolverUnionPartError{
        .code = TypeResolverUnionPartErrorCode::VOID_MEMBER,
        .message = message,
    });
  }

  return ok(type);
}

TypeResolver::ResultUnionIsSupported TypeResolver::unionAllowed(const Type *type,
                                                                bool withNull) const {
  if (withNull && !type->nonOptional()->isJson()) {
    auto message = ErrorTextTypeShape::nullUnion();

    return err(TypeResolverUnionAllowedError{
        .code = TypeResolverUnionAllowedErrorCode::NULL_MEMBER,
        .message = message,
    });
  }
  auto problem = UnionSlots::unionProblem(type);
  if (!problem.empty()) {
    auto message = ErrorTextTypeShape::noUnion(type->name(), problem);

    return err(TypeResolverUnionAllowedError{
        .code = TypeResolverUnionAllowedErrorCode::NOT_SUPPORTED,
        .message = message,
    });
  }

  return ok(type);
}

TypeResolver::ResultRestParamIsArray TypeResolver::restParam(const Type *type) const {
  if (!type->isArray() && !type->isListParam() && !type->isTuple() && !type->isError()) {
    auto message = ErrorTextTypeShape::restArray();

    return err(TypeResolverRestParamError{
        .code = TypeResolverRestParamErrorCode::NOT_AN_ARRAY,
        .message = message,
    });
  }

  return ok(type);
}

TypeResolver::ResultParamOrderIsKept TypeResolver::paramOrder(bool optionalBefore) const {
  if (optionalBefore) {
    auto message = ErrorTextTypeShape::paramOrder();

    return err(TypeResolverParamOrderError{
        .code = TypeResolverParamOrderErrorCode::REQUIRED_AFTER_OPTIONAL,
        .message = message,
    });
  }

  return ok(optionalBefore);
}

TypeResolver::ResultTupleElementType TypeResolver::tupleElement(const Type *element) const {
  if (element->isListParam()) {
    auto message = ErrorTextTypeShape::typeList(element->name());

    return err(TypeResolverTupleElementError{
        .code = TypeResolverTupleElementErrorCode::TYPE_LIST,
        .message = message,
    });
  }

  return ok(element);
}

TypeResolver::ResultTupleSpreadJoins
TypeResolver::spreadJoined(llvm::ArrayRef<const Type *> elements,
                           const std::vector<bool> &spread) const {
  auto why = std::string();
  const auto *list = ArgumentLists::spreadTuple(elements, spread, why);
  if (list == nullptr) {
    return err(TypeResolverSpreadJoinedError{
        .code = TypeResolverSpreadJoinedErrorCode::NOT_JOINED,
        .message = why,
    });
  }

  return ok(list);
}

TypeResolver::ResultOverloadsAllowed
TypeResolver::overloadAllowed(const std::vector<Overload> *overloads, llvm::StringRef name) const {
  if (overloads == nullptr) {
    auto message = ErrorTextTypeFields::signatures(name);

    return err(TypeResolverOverloadAllowedError{
        .code = TypeResolverOverloadAllowedErrorCode::NOT_AN_INTERFACE,
        .message = message,
    });
  }

  return ok(name);
}

TypeResolver::ResultUniqueSignatures
TypeResolver::newSignature(const Overload &overload, const Type *type, llvm::StringRef name) const {
  if (llvm::is_contained(overload.signatures, type)) {
    auto message = ErrorTextTypeFields::duplicate(type->name(), name);

    return err(TypeResolverNewSignatureError{
        .code = TypeResolverNewSignatureErrorCode::DUPLICATE_SIGNATURE,
        .message = message,
    });
  }

  return ok(type);
}

TypeResolver::ResultUniqueFieldNames TypeResolver::newField(llvm::ArrayRef<Field> fields,
                                                            llvm::StringRef name) const {
  if (this->hasFieldNamed(fields, name)) {
    auto message = ErrorTextTypeFields::sameField(name);

    return err(TypeResolverNewFieldError{
        .code = TypeResolverNewFieldErrorCode::DUPLICATE_FIELD,
        .message = message,
    });
  }

  return ok(name);
}

bool TypeResolver::hasFieldNamed(llvm::ArrayRef<Field> fields, llvm::StringRef name) const {
  for (const Field &field : fields) {
    if (field.name == name) {
      return true;
    }
  }

  return false;
}

bool TypeResolver::isBoundParam(const TypeRef &ref) const {
  if (ref.kind() != TypeRefKind::NAME_REF) {
    return false;
  }
  auto *bound = dyn_cast_or_null<TypeAliasDecl>(this->typeDeclarer->lookupType(ref.name));

  return bound != nullptr && bound->boundParam;
}

const Type *TypeResolver::resolveType(const TypeRef &ref) {
  const auto *type = this->resolveTypeInner(ref);
  if (type->isVoid() && this->isBoundParam(ref)) {
    return Type::undefinedTy();
  }
  auto nonVoid = this->checkNotVoid(type);
  if (nonVoid.isErr()) {
    this->state.getDiagnostics().error(ref.loc, nonVoid.error().message);

    return Type::error();
  }

  return this->standaloneType(ref, type);
}

const Type *TypeResolver::resolveTypeOrVoid(const TypeRef &ref) {
  const auto *type = this->resolveTypeInner(ref);

  return this->standaloneType(ref, type);
}

const Type *TypeResolver::standaloneType(const TypeRef &ref, const Type *type) {
  auto defined = this->checkNotUndefined(type);
  if (defined.isErr()) {
    this->state.getDiagnostics().error(ref.loc, defined.error().message);

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
      return this->literalType(ref);
    case TypeRefKind::PREDICATE_REF:
      return this->rejectPredicate(ref);
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
    case TypeRefKind::OBJECT_REF:
      return this->resolveObject(ref);
    case TypeRefKind::TUPLE_REF:
      return this->resolveTuple(ref);
  }
  llvm_unreachable("unknown type reference");
}

const Type *TypeResolver::rejectPredicate(const TypeRef &ref) {
  auto message = ErrorTextTypeShape::predicate(ref.name);
  this->state.getDiagnostics().error(ref.loc, message);

  return Type::error();
}

const Type *TypeResolver::resolveObject(const TypeRef &ref) {
  auto fields = this->resolveFields(ref);

  return Type::object(fields);
}

const Type *TypeResolver::instantiate(ClassDecl *classDecl, std::vector<const Type *> args,
                                      llvm::SMLoc loc) {
  auto made = this->genericClassInstantiator->instantiateClass(classDecl, std::move(args), loc);
  if (made.isOk()) {
    return made.value()->type;
  }
  GenericClassInstantiatorInstantiateClassErrorCode code = made.error().code;
  if (code != GenericClassInstantiatorInstantiateClassErrorCode::ALREADY_REPORTED) {
    bool isEndless = code == GenericClassInstantiatorInstantiateClassErrorCode::ENDLESS_INSTANCES;
    llvm::SMLoc at = isEndless ? classDecl->loc() : loc;
    this->state.getDiagnostics().error(at, made.error().message);
  }

  return Type::error();
}

const Type *TypeResolver::resolveDeclared(const TypeRef &ref, Stmt *decl) {
  auto *alias = dyn_cast<TypeAliasDecl>(decl);
  auto *classDecl = dyn_cast<ClassDecl>(decl);

  if (alias != nullptr && alias->typeParams.empty()) {
    return this->typeDeclarer->resolveAlias(alias);
  }
  if (alias != nullptr) {
    return this->aliasInstantiator->instantiateAlias(alias, {}, ref.loc);
  }
  if (classDecl != nullptr && classDecl->isGeneric()) {
    return this->instantiate(classDecl, {}, ref.loc);
  }
  if (classDecl != nullptr) {
    return this->classValueChecker->finalClass(classDecl)->type;
  }

  return this->typeDeclarer->enumType(cast<EnumDecl>(decl));
}

TypeResolver::ResultIntlTypeResolved TypeResolver::intlType(llvm::StringRef name) const {
  ClassDecl *locale = this->localeClass(name);
  if (locale != nullptr) {
    return ok(this->classValueChecker->finalClass(locale)->type);
  }
  auto shortName = name.drop_front(kIntlPrefix.size());
  const auto *intl = this->intlChecker->intlTypeNamed(shortName);
  if (intl == nullptr) {
    auto message = ErrorTextTypeName::notFound(name);

    return err(TypeResolverIntlTypeError{
        .code = TypeResolverIntlTypeErrorCode::NOT_FOUND,
        .message = message,
    });
  }

  return ok(intl);
}

ClassDecl *TypeResolver::localeClass(llvm::StringRef name) const {
  if (name != kIntlLocaleType) {
    return nullptr;
  }
  auto *decl = this->typeDeclarer->lookupType(LibraryModuleNames::kIntlLocaleName);

  return dyn_cast_or_null<ClassDecl>(decl);
}

const Type *TypeResolver::builtinNamed(llvm::StringRef name) const {
  if (TypeNameTable::isName(name, TypeName::TEMPLATE_STRINGS_ARRAY)) {
    const auto *text = Type::string();
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
  const auto *bytes = this->bytesChecker->bytesTypeNamed(name);
  if (bytes != nullptr) {
    return bytes;
  }
  const auto *node = this->builtinModuleChecker->nodeName(name);
  bool isInterface = node != nullptr && node->name == kReadlineInterface;
  if (isInterface && node->module == NodeModule::Readline) {
    return Type::runtimeObject(Type::Runtime::Readline);
  }
  if (isInterface && node->module == NodeModule::ReadlinePromises) {
    return Type::runtimeObject(Type::Runtime::ReadlinePromises);
  }
  const auto *crypto = this->cryptoChecker->cryptoTypeNamed(name);
  if (crypto != nullptr) {
    return crypto;
  }
  if (TypeNameTable::isName(name, TypeName::REG_EXP_MATCH_ARRAY)) {
    return Type::match(-1, {}, nullptr, false);
  }

  return nullptr;
}

const Type *TypeResolver::resolveDict(const TypeRef &ref) {
  auto dict = this->dictChecker->resolveDictType(ref);
  if (dict.isOk()) {
    return dict.value();
  }
  if (dict.error().code == DictCheckerResolveDictTypeErrorCode::NOT_A_DICT) {
    return nullptr;
  }
  bool isOnKey = this->dictChecker->isKeyArgument(dict.error().code);
  llvm::SMLoc at = isOnKey ? ref.args[0].loc : ref.loc;
  this->state.getDiagnostics().error(at, dict.error().message);

  return Type::error();
}

std::string TypeResolver::missingText(llvm::StringRef name) const {
  if (TypeNameTable::isName(name, TypeName::ARRAY)) {
    return ErrorTextTypeName::arrayArg();
  }
  auto result = this->resultTypes->findResultType(name);
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
    auto what = ErrorTextTypeName::bigintType();
    auto bigint = ErrorTextBigInt::none(what);
    this->state.getDiagnostics().error(ref.loc, bigint);
    return Type::error();
  }
  bool isBigintArray = TypeNameTable::isName(ref.name, TypeName::BIG_INT64_ARRAY) ||
                       TypeNameTable::isName(ref.name, TypeName::BIG_UINT64_ARRAY);
  if (isBigintArray) {
    auto what = ErrorTextTypeName::bigintName(ref.name);
    auto bigint = ErrorTextBigInt::none(what);
    this->state.getDiagnostics().error(ref.loc, bigint);
    return Type::error();
  }
  auto message = this->missingText(ref.name);
  this->state.getDiagnostics().error(ref.loc, message);

  return Type::error();
}

const Type *TypeResolver::resolveName(const TypeRef &ref) {
  if (ref.name.empty()) {
    return Type::error();
  }
  const auto *type = Type::fromName(ref.name);
  if (type != nullptr) {
    return type;
  }
  if (TypeNameTable::isName(ref.name, TypeName::NEVER) &&
      this->typeDeclarer->lookupType(ref.name) == nullptr) {
    return Type::never();
  }
  auto *decl = this->typeDeclarer->lookupType(ref.name);
  if (decl != nullptr) {
    return this->resolveDeclared(ref, decl);
  }
  auto [owner, member] = ref.name.split('.');
  EnumDecl *declaredEnum = this->enumOwning(owner, member);
  if (declaredEnum != nullptr) {
    return this->resolveEnumMember(ref, declaredEnum);
  }
  if (ref.name.starts_with(kIntlPrefix)) {
    return this->resolveIntl(ref);
  }
  const auto *builtin = this->builtinNamed(ref.name);
  if (builtin != nullptr) {
    return builtin;
  }
  const auto *dict = this->resolveDict(ref);
  if (dict != nullptr) {
    return dict;
  }
  if (TypeNameTable::isName(ref.name, TypeName::CONSTRUCTOR)) {
    const auto *object = Type::anyObject();
    return Type::constructorOf(object);
  }
  if (TypeNameTable::isName(ref.name, TypeName::CONSTRUCTOR_ARGS)) {
    return this->resolveConstructorArgs(ref);
  }

  return this->missingType(ref);
}

EnumDecl *TypeResolver::enumOwning(llvm::StringRef owner, llvm::StringRef member) const {
  if (member.empty()) {
    return nullptr;
  }

  return dyn_cast_or_null<EnumDecl>(this->typeDeclarer->lookupType(owner));
}

const Type *TypeResolver::resolveEnumMember(const TypeRef &ref, EnumDecl *declaredEnum) {
  auto [owner, member] = ref.name.split('.');
  this->typeDeclarer->resolveEnum(declaredEnum);
  auto found = this->enumMember(declaredEnum, owner, member);
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

const Type *TypeResolver::resolveIntl(const TypeRef &ref) {
  auto intl = this->intlType(ref.name);
  if (intl.isErr()) {
    this->state.getDiagnostics().error(ref.loc, intl.error().message);

    return Type::error();
  }

  return intl.value();
}

const Type *TypeResolver::resolveConstructorArgs(const TypeRef &ref) {
  auto args = this->classValueChecker->checkConstructorArgs();
  if (args.isErr()) {
    this->state.getDiagnostics().error(ref.loc, args.error().message);

    return Type::error();
  }

  return args.value();
}

const Type *TypeResolver::resolveArrayGeneric(const TypeRef &ref) {
  auto given = this->arrayArgument(ref);
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

const Type *TypeResolver::resolveGenericClass(const TypeRef &ref, ClassDecl *classDecl) {
  auto args = std::vector<const Type *>();
  for (const TypeRef &arg : ref.args) {
    const auto *type = this->resolveTypeOrVoid(arg);
    if (type->isError()) {
      return type;
    }
    args.push_back(type);
  }
  auto generic = this->classGeneric(classDecl, ref.name);
  if (generic.isErr()) {
    this->state.getDiagnostics().error(ref.loc, generic.error().message);
    return Type::error();
  }

  return this->instantiate(classDecl, std::move(args), ref.loc);
}

const Type *TypeResolver::resolveGenericAlias(const TypeRef &ref, TypeAliasDecl *alias) {
  auto args = std::vector<const Type *>();
  for (const TypeRef &arg : ref.args) {
    const auto *type = this->conditionalTypes->resolveTypeArgument(arg);
    if (type->isError()) {
      return type;
    }
    args.push_back(type);
  }
  auto generic = this->aliasGeneric(alias, ref.name);
  if (generic.isErr()) {
    this->state.getDiagnostics().error(ref.loc, generic.error().message);
    return Type::error();
  }

  return this->aliasInstantiator->instantiateAlias(alias, std::move(args), ref.loc);
}

const Type *TypeResolver::resolveIterator(const TypeRef &ref) {
  auto iterator = this->generatorChecker->resolveIteratorType(ref);
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
  auto promise = this->asyncChecker->resolvePromiseType(ref);
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
  auto utility = this->utilityTypes->resolveUtilityType(ref);
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
  auto arity = this->descriptorArity(ref);
  if (arity.isErr()) {
    this->state.getDiagnostics().error(ref.loc, arity.error().message);
    return Type::error();
  }
  const auto *params = this->resolveType(ref.args[0]);
  const auto *returned = this->resolveTypeOrVoid(ref.args[1]);
  if (params->isError() || returned->isError()) {
    return Type::error();
  }
  auto list = this->descriptorParams(params);
  if (list.isErr()) {
    this->state.getDiagnostics().error(ref.args[0].loc, list.error().message);
    return Type::error();
  }

  return this->decoratorChecker->methodDescriptorType(params, returned);
}

const Type *TypeResolver::resolveConstructor(const TypeRef &ref) {
  auto constructor = this->classValueChecker->resolveConstructorType(ref);
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
  const auto *classType = this->resolveType(ref.args[0]);
  if (classType->isError()) {
    return classType;
  }
  auto made = this->classInstance(classType);
  if (made.isErr()) {
    this->state.getDiagnostics().error(ref.args[0].loc, made.error().message);
    return Type::error();
  }

  return made.value();
}

const Type *TypeResolver::resolveGeneric(const TypeRef &ref) {
  auto *decl = this->typeDeclarer->lookupType(ref.name);
  auto *classDecl = dyn_cast_or_null<ClassDecl>(decl);
  auto *alias = dyn_cast_or_null<TypeAliasDecl>(decl);
  const auto *bytes = this->bytesChecker->bytesTypeNamed(ref.name);
  bool isTypedArray = bytes != nullptr && bytes->isTypedArray() && decl == nullptr;
  if (isTypedArray) {
    return this->resolveTypedArray(ref, bytes);
  }
  auto result = this->resultTypes->findResultType(ref.name);
  if (result.has_value()) {
    return this->resultTypes->resolveResultType(ref, *result);
  }
  const auto *builtin = TypeNameTable::find(ref.name);
  bool isArray = builtin != nullptr && builtin->form.is<ArrayTypeName>();
  if (isArray) {
    return this->resolveArrayGeneric(ref);
  }
  if (classDecl != nullptr) {
    return this->resolveGenericClass(ref, classDecl);
  }
  if (alias != nullptr) {
    return this->resolveGenericAlias(ref, alias);
  }
  const auto *iterator = this->resolveIterator(ref);
  if (iterator != nullptr) {
    return iterator;
  }
  const auto *dict = this->resolveDict(ref);
  if (dict != nullptr) {
    return dict;
  }
  const auto *promise = this->resolvePromise(ref);
  if (promise != nullptr) {
    return promise;
  }
  if (TypeNameTable::isName(ref.name, TypeName::METHOD_DESCRIPTOR)) {
    return this->resolveDescriptor(ref);
  }
  const auto *utility = this->resolveUtility(ref);
  if (utility != nullptr) {
    return utility;
  }
  if (TypeNameTable::isName(ref.name, TypeName::CONSTRUCTOR)) {
    return this->resolveConstructor(ref);
  }
  if (TypeNameTable::isName(ref.name, TypeName::INSTANCE_TYPE) && ref.args.size() == 1) {
    return this->resolveInstanceType(ref);
  }
  auto message = ErrorTextGenericRef::noSupport(ref.name);
  this->state.getDiagnostics().error(ref.loc, message);

  return Type::error();
}

const Type *TypeResolver::resolveTypedArray(const TypeRef &ref, const Type *bytes) {
  auto buffer = this->bufferArgument(ref, bytes);
  if (buffer.isErr()) {
    this->state.getDiagnostics().error(ref.loc, buffer.error().message);

    return Type::error();
  }

  return buffer.value();
}

const Type *TypeResolver::resolveArray(const TypeRef &ref) {
  const auto *element = this->resolveTypeInner(ref.args[0]);
  auto allowed = this->arrayElement(element);
  if (allowed.isErr()) {
    this->state.getDiagnostics().error(ref.loc, allowed.error().message);
    return Type::error();
  }

  return Type::array(element);
}

const Type *TypeResolver::resolveUnion(const TypeRef &ref) {
  bool isWithNull = false;
  auto members = std::vector<const Type *>();
  for (const TypeRef &member : ref.args) {
    if (this->isNullRef(member)) {
      isWithNull = true;
    } else {
      const Type *type = this->resolveUnionMember(ref, member);
      if (type->isError()) {
        return type;
      }
      members.push_back(type);
    }
  }
  const auto *type = Type::unionOf(members);
  auto supported = this->unionAllowed(type, isWithNull);
  if (supported.isErr()) {
    this->state.getDiagnostics().error(ref.loc, supported.error().message);
    return Type::error();
  }

  return type;
}

bool TypeResolver::isNullRef(const TypeRef &ref) const {
  return ref.kind() == TypeRefKind::NAME_REF &&
         TypeNameTable::isName(ref.name, TypeName::NULL_TYPE);
}

const Type *TypeResolver::resolveUnionMember(const TypeRef &ref, const TypeRef &member) {
  const auto *type = this->resolveTypeInner(member);
  if (type->isError()) {
    return type;
  }
  auto part = this->unionPart(type);
  if (part.isErr()) {
    this->state.getDiagnostics().error(ref.loc, part.error().message);

    return Type::error();
  }

  return type;
}

const Type *TypeResolver::resolveOperator(const TypeRef &ref) {
  auto resolved = this->typeOperators->resolveTypeOp(ref);
  if (resolved.isOk()) {
    return resolved.value();
  }
  auto at = this->operatorErrorLoc(ref, resolved.error().code);
  this->state.getDiagnostics().error(at, resolved.error().message);

  return Type::error();
}

llvm::SMLoc TypeResolver::operatorErrorLoc(const TypeRef &ref,
                                           TypeOperatorsResolveTypeOpErrorCode code) const {
  switch (code) {
    case TypeOperatorsResolveTypeOpErrorCode::RENAME_NOT_STRING:
      return ref.args[2].loc;
    case TypeOperatorsResolveTypeOpErrorCode::KEYS_NOT_STRINGS:
      return ref.args[0].loc;
    case TypeOperatorsResolveTypeOpErrorCode::NOT_AN_OBJECT_TYPE:
    case TypeOperatorsResolveTypeOpErrorCode::NO_SUCH_MEMBER:
    case TypeOperatorsResolveTypeOpErrorCode::TYPE_OF_UNKNOWN:
    case TypeOperatorsResolveTypeOpErrorCode::NO_SUCH_FIELD:
      return ref.loc;
  }

  return ref.loc;
}

const Type *TypeResolver::resolveIntersection(const TypeRef &ref) {
  auto joined = this->conditionalTypes->resolveIntersection(ref);
  if (joined.isErr()) {
    this->state.getDiagnostics().error(ref.loc, joined.error().message);
    return Type::error();
  }

  return joined.value();
}

TypeResolver::ResultInferTypeBinding TypeResolver::inferBinding(const TypeRef &ref) const {
  auto *binding = dyn_cast_or_null<TypeAliasDecl>(this->typeDeclarer->lookupType(ref.name));
  if (!this->state.getTypeContext().isInsideExtendsClause() || binding == nullptr ||
      binding->resolved == nullptr) {
    auto message = ErrorTextTypeShape::inferOnly();

    return err(TypeResolverInferBindingError{
        .code = TypeResolverInferBindingErrorCode::OUTSIDE_EXTENDS,
        .message = message,
    });
  }

  return ok(binding->resolved);
}

const Type *TypeResolver::resolveInfer(const TypeRef &ref) {
  auto binding = this->inferBinding(ref);
  if (binding.isErr()) {
    this->state.getDiagnostics().error(ref.loc, binding.error().message);
    return Type::error();
  }

  return binding.value();
}

const Type *TypeResolver::resolveFunctionType(const TypeRef &ref) {
  auto params = this->resolveParams(ref);
  const Type *returnType = this->resolveReturned(ref.args.back());
  if (!params.isValid || returnType->isError()) {
    return Type::error();
  }
  bool hasThis = this->hasThisParam(ref);

  return Type::function(params.types, params.minArgs, ref.rest(), returnType, hasThis);
}

bool TypeResolver::hasThisParam(const TypeRef &ref) const {
  size_t count = ref.args.size() - 1;
  bool isNamedThis = count > 0 && !ref.names().empty() && ref.names()[0] == kThisParam;
  bool isOnlyRest = ref.rest() && count == 1;

  return isNamedThis && !ref.optional()[0] && !isOnlyRest;
}

TypeResolver::FunctionParams TypeResolver::resolveParams(const TypeRef &ref) {
  auto params = FunctionParams{.types = {}, .minArgs = 0, .hasOptional = false, .isValid = true};
  size_t count = ref.args.size() - 1;
  for (size_t index = 0; index < count; ++index) {
    bool isRest = ref.rest() && index + 1 == count;
    const Type *type = this->resolveParam(ref.args[index], isRest);
    params.isValid = params.isValid && !type->isError();
    if (isRest) {
      this->addRestParam(params, ref.args[index], type);
    } else if (ref.optional()[index]) {
      this->addOptionalParam(params, type);
    } else {
      this->addRequiredParam(params, ref.args[index], type);
    }
  }

  return params;
}

const Type *TypeResolver::resolveParam(const TypeRef &param, bool isRest) {
  this->state.getTypeContext().enterListContext(isRest);
  const auto *type = this->resolveType(param);
  this->state.getTypeContext().leaveListContext();

  return type;
}

void TypeResolver::addRestParam(FunctionParams &params, const TypeRef &param, const Type *type) {
  auto list = this->restParam(type);
  if (list.isErr()) {
    this->state.getDiagnostics().error(param.loc, list.error().message);
    params.isValid = false;
  }
  params.types.push_back(type);
}

void TypeResolver::addOptionalParam(FunctionParams &params, const Type *type) const {
  params.hasOptional = true;
  params.types.push_back(Type::optional(type));
}

void TypeResolver::addRequiredParam(FunctionParams &params, const TypeRef &param,
                                    const Type *type) {
  auto order = this->paramOrder(params.hasOptional);
  if (order.isErr()) {
    this->state.getDiagnostics().error(param.loc, order.error().message);
    params.isValid = false;
  }
  params.types.push_back(type);
  params.minArgs = params.types.size();
}

const Type *TypeResolver::resolveReturned(const TypeRef &returned) {
  if (returned.kind() != TypeRefKind::PREDICATE_REF) {
    return this->resolveTypeOrVoid(returned);
  }
  if (!returned.asserts()) {
    return Type::boolTy();
  }

  return this->rejectAsserts(returned);
}

const Type *TypeResolver::rejectAsserts(const TypeRef &returned) {
  auto shown = returned.name.str();
  bool hasType = !returned.args.empty();
  if (hasType) {
    const auto *type = this->resolveType(returned.args[0]);
    shown += " is " + type->name().str();
  }
  auto message = ErrorTextAsserts::notAllowed(shown, hasType, "");
  this->state.getDiagnostics().error(returned.loc, message);

  return Type::error();
}

TypeResolver::ResultTupleLengthFixed TypeResolver::fixedLength(const Type *list) const {
  if (list->isTuple() && list->tupleRest() != nullptr &&
      !this->state.getTypeContext().isInsideListContext() &&
      !this->state.getTypeContext().isInsideExtendsClause()) {
    auto message = ErrorTextTypeShape::tupleRest();

    return err(TypeResolverFixedLengthError{
        .code = TypeResolverFixedLengthErrorCode::REST_ELEMENT,
        .message = message,
    });
  }

  return ok(list);
}

const Type *TypeResolver::resolveSpreadTuple(const TypeRef &ref,
                                             const std::vector<const Type *> &elements) {
  if (this->isSpreadWaiting(ref, elements) && !this->hasErrorType(elements)) {
    return this->deferredSpread(ref, elements);
  }
  auto joined = this->spreadJoined(elements, ref.spread());
  if (joined.isErr()) {
    this->state.getDiagnostics().error(ref.loc, joined.error().message);
    return Type::error();
  }
  const Type *list = joined.value();
  auto fixed = this->fixedLength(list);
  if (fixed.isErr()) {
    this->state.getDiagnostics().error(ref.loc, fixed.error().message);
    return Type::error();
  }

  return list;
}

bool TypeResolver::isSpreadWaiting(const TypeRef &ref,
                                   llvm::ArrayRef<const Type *> elements) const {
  for (size_t index = 0; index < elements.size(); ++index) {
    bool isOpen = elements[index]->typeOp() != TypeOp::None && !elements[index]->isListParam();
    if (ref.spread()[index] && isOpen) {
      return true;
    }
  }

  return false;
}

bool TypeResolver::hasErrorType(llvm::ArrayRef<const Type *> types) const {
  for (const Type *type : types) {
    if (type->isError()) {
      return true;
    }
  }

  return false;
}

const Type *TypeResolver::deferredSpread(const TypeRef &ref,
                                         llvm::ArrayRef<const Type *> elements) {
  auto operands = std::vector<const Type *>();
  for (size_t index = 0; index < elements.size(); ++index) {
    operands.push_back(Type::boolLiteral(ref.spread()[index]));
    operands.push_back(elements[index]);
  }
  const auto *spread = Type::typeOp(TypeOp::Spread, operands);

  return this->typeOperators->evalTypeOp(spread, operands);
}

const Type *TypeResolver::resolveTuple(const TypeRef &ref) {
  auto elements = std::vector<const Type *>();
  for (const TypeRef &element : ref.args) {
    const auto *type = this->resolveType(element);
    elements.push_back(type);
  }
  if (llvm::is_contained(ref.spread(), true)) {
    return this->resolveSpreadTuple(ref, elements);
  }
  for (size_t index = 0; index < elements.size(); ++index) {
    auto element = this->tupleElement(elements[index]);
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
  auto fields = std::vector<Field>();
  for (size_t index = 0; index < ref.args.size(); ++index) {
    if (this->isOverloaded(ref, index)) {
      this->addSignature(ref, index, overloads);
    } else {
      this->addField(ref, index, fields);
    }
  }

  return fields;
}

void TypeResolver::addSignature(const TypeRef &ref, size_t index,
                                std::vector<Overload> *overloads) {
  llvm::StringRef name = ref.names()[index];
  auto allowed = this->overloadAllowed(overloads, name);
  if (allowed.isErr()) {
    this->state.getDiagnostics().error(ref.locs()[index], allowed.error().message);
    return;
  }
  const auto *type = this->resolveType(ref.args[index]);
  Overload &overload = this->overloadNamed(*overloads, name);
  auto unique = this->newSignature(overload, type, name);
  if (unique.isErr()) {
    this->state.getDiagnostics().error(ref.locs()[index], unique.error().message);
    return;
  }
  if (!type->isError()) {
    overload.signatures.push_back(type);
  }
}

Overload &TypeResolver::overloadNamed(std::vector<Overload> &overloads,
                                      llvm::StringRef name) const {
  for (Overload &overload : overloads) {
    if (overload.name == name) {
      return overload;
    }
  }
  overloads.push_back(Overload{name.str(), {}});

  return overloads.back();
}

void TypeResolver::addField(const TypeRef &ref, size_t index, std::vector<Field> &fields) {
  llvm::StringRef name = ref.names()[index];
  auto unique = this->newField(fields, name);
  if (unique.isErr()) {
    this->state.getDiagnostics().error(ref.locs()[index], unique.error().message);
    return;
  }
  const auto *written = this->resolveType(ref.args[index]);
  const Type *type = ref.optional()[index] ? Type::optional(written) : written;
  fields.push_back({name.str(), type, ref.readonly()[index]});
}

}
