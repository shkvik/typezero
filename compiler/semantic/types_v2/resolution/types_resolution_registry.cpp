#include "compiler/semantic/types_v2/resolution/types_resolution_registry.h"
#include "compiler/program/ast/ast.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/program/library/builtin_names/keyword_table/keyword_table.h"
#include "compiler/program/library/builtin_names/type_name_table/type_name_table.h"
#include "compiler/program/library/crypto/crypto_class_table/crypto_class_table.h"
#include "compiler/program/library/intl/intl_service_table/intl_service_table.h"
#include "compiler/program/library/intl/intl_type_name_table/intl_type_name_table.h"
#include "compiler/program/library/library_module_names/library_module_names.h"
#include "compiler/program/to_utf16/to_utf16.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/flow/predicate_checker/predicate_checker_errors.h"
#include "compiler/semantic/library/decimal/decimal_checker/decimal_checker_errors.h"
#include "compiler/semantic/types/typeops_check_helpers/typeops_check_helpers.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MemoryBuffer.h"
#include <cmath>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;

namespace tz {

namespace {

constexpr double kMaxWholeNumber = 9007199254740992.0;
constexpr llvm::StringLiteral kIntlPrefix = "Intl.";
constexpr Type::Bytes kNamedBytes[] = {
    Type::Bytes::Uint8,        Type::Bytes::Int8,
    Type::Bytes::Uint8Clamped, Type::Bytes::Int16,
    Type::Bytes::Uint16,       Type::Bytes::Int32,
    Type::Bytes::Uint32,       Type::Bytes::Float16,
    Type::Bytes::Float32,      Type::Bytes::Float64,
    Type::Bytes::Buffer,       Type::Bytes::ArrayBuffer,
    Type::Bytes::DataView,     Type::Bytes::TextEncoder,
    Type::Bytes::TextDecoder,  Type::Bytes::FatalTextDecoder,
};

}

TypesResolutionRegistry::TypesResolutionRegistry(SemanticState &owner)
    : state(owner) {}

const Type *TypesResolutionRegistry::literalType(const TypeRef &ref) const {
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

bool TypesResolutionRegistry::isMethodAt(const TypeRef &ref, size_t index) const {
  return index < ref.methods().size() && ref.methods()[index];
}

bool TypesResolutionRegistry::isOverloaded(const TypeRef &ref, size_t index) const {
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

TypesResolutionRegistry::ResultStandaloneUsable
TypesResolutionRegistry::checkNotVoid(const Type *type) const {
  if (type->isVoid()) {
    auto message = ErrorTextTypeUse::voidType();

    return err(TypesResolutionRegistryStandaloneError{
        .code = TypesResolutionRegistryStandaloneErrorCode::VOID_TYPE,
        .message = message,
    });
  }

  return ok(type);
}

TypesResolutionRegistry::ResultStandaloneUsable
TypesResolutionRegistry::checkNotUndefined(const Type *type) const {
  if (type->isUndefined()) {
    auto message = ErrorTextTypeUse::undefined();

    return err(TypesResolutionRegistryStandaloneError{
        .code = TypesResolutionRegistryStandaloneErrorCode::UNDEFINED_ALONE,
        .message = message,
    });
  }

  return ok(type);
}

TypesResolutionRegistry::ResultEnumMemberExists
TypesResolutionRegistry::enumMember(const EnumDecl *declaredEnum, llvm::StringRef owner,
                                    llvm::StringRef member) const {
  const auto *found = declaredEnum->find(member);
  if (found == nullptr) {
    auto message = ErrorTextTypeName::enumMember(owner, member);

    return err(TypesResolutionRegistryEnumMemberError{
        .code = TypesResolutionRegistryEnumMemberErrorCode::NO_SUCH_MEMBER,
        .message = message,
    });
  }

  return ok(found);
}

TypesResolutionRegistry::ResultArrayBufferGiven
TypesResolutionRegistry::bufferArgument(const TypeRef &ref, const Type *bytes) const {
  bool isSingle = ref.args.size() == 1 && ref.args[0].kind() == TypeRefKind::NAME_REF;
  bool isBuffer =
      isSingle && (TypeNameTable::isName(ref.args[0].name, TypeName::ARRAY_BUFFER) ||
                   TypeNameTable::isName(ref.args[0].name, TypeName::ARRAY_BUFFER_LIKE));
  if (!isBuffer) {
    auto message = ErrorTextGenericRef::bufferArg(ref.name);

    return err(TypesResolutionRegistryBufferArgumentError{
        .code = TypesResolutionRegistryBufferArgumentErrorCode::NOT_ARRAY_BUFFER,
        .message = message,
    });
  }

  return ok(bytes);
}

TypesResolutionRegistry::ResultElementTypeGiven
TypesResolutionRegistry::arrayArgument(const TypeRef &ref) const {
  if (ref.args.size() != 1) {
    auto message = ErrorTextGenericRef::oneArg(ref.name);

    return err(TypesResolutionRegistryArrayArgumentError{
        .code = TypesResolutionRegistryArrayArgumentErrorCode::NOT_ONE_ARGUMENT,
        .message = message,
    });
  }

  return ok(&ref.args[0]);
}

TypesResolutionRegistry::ResultGenericClassUsed
TypesResolutionRegistry::classGeneric(const ClassDecl *classDecl, llvm::StringRef name) const {
  if (!classDecl->isGeneric()) {
    auto message = ErrorTextGenericRef::plainClass(name);

    return err(TypesResolutionRegistryClassGenericError{
        .code = TypesResolutionRegistryClassGenericErrorCode::NOT_GENERIC,
        .message = message,
    });
  }

  return ok(classDecl);
}

TypesResolutionRegistry::ResultGenericAliasUsed
TypesResolutionRegistry::aliasGeneric(const TypeAliasDecl *alias, llvm::StringRef name) const {
  if (alias->typeParams.empty()) {
    auto message = ErrorTextGenericRef::plainAlias(name);

    return err(TypesResolutionRegistryAliasGenericError{
        .code = TypesResolutionRegistryAliasGenericErrorCode::NOT_GENERIC,
        .message = message,
    });
  }

  return ok(alias);
}

TypesResolutionRegistry::ResultTwoTypeArguments
TypesResolutionRegistry::descriptorArity(const TypeRef &ref) const {
  if (ref.args.size() != 2) {
    auto message = ErrorTextGenericRef::descriptor();

    return err(TypesResolutionRegistryDescriptorArityError{
        .code = TypesResolutionRegistryDescriptorArityErrorCode::NOT_TWO_ARGUMENTS,
        .message = message,
    });
  }

  return ok(&ref);
}

TypesResolutionRegistry::ResultParamTypesAsList
TypesResolutionRegistry::descriptorParams(const Type *params) const {
  if (!params->isTuple() && !params->isListParam()) {
    auto message = ErrorTextGenericRef::paramList();

    return err(TypesResolutionRegistryDescriptorParamsError{
        .code = TypesResolutionRegistryDescriptorParamsErrorCode::NOT_A_LIST,
        .message = message,
    });
  }

  return ok(params);
}

TypesResolutionRegistry::ResultInstanceOfAClass
TypesResolutionRegistry::classInstance(const Type *classType) const {
  const Type *made = classType->isClassRef() ? this->constructedBy(classType) : nullptr;
  if (made == nullptr) {
    auto message = ErrorTextGenericRef::instanceOf(classType->name());

    return err(TypesResolutionRegistryClassInstanceError{
        .code = TypesResolutionRegistryClassInstanceErrorCode::NOT_A_CLASS,
        .message = message,
    });
  }

  return ok(made);
}

TypesResolutionRegistry::ResultArrayElementType
TypesResolutionRegistry::arrayElement(const Type *element) const {
  if (element->isVoid() || element->isUndefined()) {
    auto message = ErrorTextTypeShape::arrayOf(element->name());

    return err(TypesResolutionRegistryArrayElementError{
        .code = TypesResolutionRegistryArrayElementErrorCode::NOT_SUPPORTED,
        .message = message,
    });
  }

  return ok(element);
}

TypesResolutionRegistry::ResultNonVoidUnionPart
TypesResolutionRegistry::unionPart(const Type *type) const {
  if (type->isVoid()) {
    auto message = ErrorTextTypeShape::voidUnion();

    return err(TypesResolutionRegistryUnionPartError{
        .code = TypesResolutionRegistryUnionPartErrorCode::VOID_MEMBER,
        .message = message,
    });
  }

  return ok(type);
}

TypesResolutionRegistry::ResultUnionIsSupported
TypesResolutionRegistry::unionAllowed(const Type *type, bool withNull) const {
  if (withNull && !type->nonOptional()->isJson()) {
    auto message = ErrorTextTypeShape::nullUnion();

    return err(TypesResolutionRegistryUnionAllowedError{
        .code = TypesResolutionRegistryUnionAllowedErrorCode::NULL_MEMBER,
        .message = message,
    });
  }
  auto problem = UnionSlots::unionProblem(type);
  if (!problem.empty()) {
    auto message = ErrorTextTypeShape::noUnion(type->name(), problem);

    return err(TypesResolutionRegistryUnionAllowedError{
        .code = TypesResolutionRegistryUnionAllowedErrorCode::NOT_SUPPORTED,
        .message = message,
    });
  }

  return ok(type);
}

TypesResolutionRegistry::ResultRestParamIsArray
TypesResolutionRegistry::restParam(const Type *type) const {
  if (!type->isArray() && !type->isListParam() && !type->isTuple() && !type->isError()) {
    auto message = ErrorTextTypeShape::restArray();

    return err(TypesResolutionRegistryRestParamError{
        .code = TypesResolutionRegistryRestParamErrorCode::NOT_AN_ARRAY,
        .message = message,
    });
  }

  return ok(type);
}

TypesResolutionRegistry::ResultParamOrderIsKept
TypesResolutionRegistry::paramOrder(bool optionalBefore) const {
  if (optionalBefore) {
    auto message = ErrorTextTypeShape::paramOrder();

    return err(TypesResolutionRegistryParamOrderError{
        .code = TypesResolutionRegistryParamOrderErrorCode::REQUIRED_AFTER_OPTIONAL,
        .message = message,
    });
  }

  return ok(optionalBefore);
}

TypesResolutionRegistry::ResultTupleElementType
TypesResolutionRegistry::tupleElement(const Type *element) const {
  if (element->isListParam()) {
    auto message = ErrorTextTypeShape::typeList(element->name());

    return err(TypesResolutionRegistryTupleElementError{
        .code = TypesResolutionRegistryTupleElementErrorCode::TYPE_LIST,
        .message = message,
    });
  }

  return ok(element);
}

TypesResolutionRegistry::ResultTupleSpreadJoins
TypesResolutionRegistry::spreadJoined(llvm::ArrayRef<const Type *> elements,
                                      const std::vector<bool> &spread) const {
  auto why = std::string();
  const auto *list = ArgumentLists::spreadTuple(elements, spread, why);
  if (list == nullptr) {
    return err(TypesResolutionRegistrySpreadJoinedError{
        .code = TypesResolutionRegistrySpreadJoinedErrorCode::NOT_JOINED,
        .message = why,
    });
  }

  return ok(list);
}

TypesResolutionRegistry::ResultOverloadsAllowed
TypesResolutionRegistry::overloadAllowed(const std::vector<Overload> *overloads,
                                         llvm::StringRef name) const {
  if (overloads == nullptr) {
    auto message = ErrorTextTypeFields::signatures(name);

    return err(TypesResolutionRegistryOverloadAllowedError{
        .code = TypesResolutionRegistryOverloadAllowedErrorCode::NOT_AN_INTERFACE,
        .message = message,
    });
  }

  return ok(name);
}

TypesResolutionRegistry::ResultUniqueSignatures
TypesResolutionRegistry::newSignature(const Overload &overload, const Type *type,
                                      llvm::StringRef name) const {
  if (llvm::is_contained(overload.signatures, type)) {
    auto message = ErrorTextTypeFields::duplicate(type->name(), name);

    return err(TypesResolutionRegistryNewSignatureError{
        .code = TypesResolutionRegistryNewSignatureErrorCode::DUPLICATE_SIGNATURE,
        .message = message,
    });
  }

  return ok(type);
}

TypesResolutionRegistry::ResultUniqueFieldNames
TypesResolutionRegistry::newField(llvm::ArrayRef<Field> fields, llvm::StringRef name) const {
  if (this->hasFieldNamed(fields, name)) {
    auto message = ErrorTextTypeFields::sameField(name);

    return err(TypesResolutionRegistryNewFieldError{
        .code = TypesResolutionRegistryNewFieldErrorCode::DUPLICATE_FIELD,
        .message = message,
    });
  }

  return ok(name);
}

bool TypesResolutionRegistry::hasFieldNamed(llvm::ArrayRef<Field> fields,
                                            llvm::StringRef name) const {
  for (const Field &field : fields) {
    if (field.name == name) {
      return true;
    }
  }

  return false;
}

bool TypesResolutionRegistry::isBoundParam(const TypeRef &ref) const {
  if (ref.kind() != TypeRefKind::NAME_REF) {
    return false;
  }
  auto *bound = dyn_cast_or_null<TypeAliasDecl>(this->state.findTypeDeclaration(ref.name));

  return bound != nullptr && bound->boundParam;
}

const Type *TypesResolutionRegistry::standaloneType(const TypeRef &ref, const Type *type) {
  auto defined = this->checkNotUndefined(type);
  if (defined.isErr()) {
    this->state.diagnostics.error(ref.loc, defined.error().message);

    return Type::error();
  }

  return type;
}

const Type *TypesResolutionRegistry::rejectPredicate(const TypeRef &ref) {
  auto message = ErrorTextTypeShape::predicate(ref.name);
  this->state.diagnostics.error(ref.loc, message);

  return Type::error();
}

ClassDecl *TypesResolutionRegistry::localeClass(llvm::StringRef name) const {
  auto localeName = IntlTypeNameTable::getName(IntlTypeName::LOCALE);
  if (name != IntlServiceTable::getQualifiedName(localeName)) {
    return nullptr;
  }
  auto *decl = this->state.findTypeDeclaration(LibraryModuleNames::kIntlLocaleName);

  return dyn_cast_or_null<ClassDecl>(decl);
}

const Type *TypesResolutionRegistry::builtinNamed(llvm::StringRef name) const {
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
  const auto *bytes = this->bytesTypeNamed(name);
  if (bytes != nullptr) {
    return bytes;
  }
  const auto *node = this->state.findNodeName(name);
  auto interfaceName = Type::runtimeObject(Type::Runtime::Readline)->name();
  bool isInterface = node != nullptr && node->name == interfaceName;
  if (isInterface && node->module == NodeModule::Readline) {
    return Type::runtimeObject(Type::Runtime::Readline);
  }
  if (isInterface && node->module == NodeModule::ReadlinePromises) {
    return Type::runtimeObject(Type::Runtime::ReadlinePromises);
  }
  const auto *crypto = this->cryptoTypeNamed(name);
  if (crypto != nullptr) {
    return crypto;
  }
  if (TypeNameTable::isName(name, TypeName::REG_EXP_MATCH_ARRAY)) {
    return Type::match(-1, {}, nullptr, false);
  }

  return nullptr;
}

std::string TypesResolutionRegistry::missingText(llvm::StringRef name) const {
  if (TypeNameTable::isName(name, TypeName::ARRAY)) {
    return ErrorTextTypeName::arrayArg();
  }
  auto result = this->resultTypeNamed(name);
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

const Type *TypesResolutionRegistry::missingType(const TypeRef &ref) {
  if (TypeNameTable::isName(ref.name, TypeName::BIGINT)) {
    auto what = ErrorTextTypeName::bigintType();
    auto bigint = ErrorTextBigInt::none(what);
    this->state.diagnostics.error(ref.loc, bigint);
    return Type::error();
  }
  bool isBigintArray = TypeNameTable::isName(ref.name, TypeName::BIG_INT64_ARRAY) ||
                       TypeNameTable::isName(ref.name, TypeName::BIG_UINT64_ARRAY);
  if (isBigintArray) {
    auto what = ErrorTextTypeName::bigintName(ref.name);
    auto bigint = ErrorTextBigInt::none(what);
    this->state.diagnostics.error(ref.loc, bigint);
    return Type::error();
  }
  auto message = this->missingText(ref.name);
  this->state.diagnostics.error(ref.loc, message);

  return Type::error();
}

EnumDecl *TypesResolutionRegistry::enumOwning(llvm::StringRef owner, llvm::StringRef member) const {
  if (member.empty()) {
    return nullptr;
  }

  return dyn_cast_or_null<EnumDecl>(this->state.findTypeDeclaration(owner));
}

const Type *TypesResolutionRegistry::resolveTypedArray(const TypeRef &ref, const Type *bytes) {
  auto buffer = this->bufferArgument(ref, bytes);
  if (buffer.isErr()) {
    this->state.diagnostics.error(ref.loc, buffer.error().message);

    return Type::error();
  }

  return buffer.value();
}

bool TypesResolutionRegistry::isNullRef(const TypeRef &ref) const {
  return ref.kind() == TypeRefKind::NAME_REF &&
         TypeNameTable::isName(ref.name, TypeName::NULL_TYPE);
}

TypesResolutionRegistry::ResultInferTypeBinding
TypesResolutionRegistry::inferBinding(const TypeRef &ref) const {
  auto *binding = dyn_cast_or_null<TypeAliasDecl>(this->state.findTypeDeclaration(ref.name));
  if (!this->state.types.context.isInsideExtendsClause() || binding == nullptr ||
      binding->resolved == nullptr) {
    auto message = ErrorTextTypeShape::inferOnly();

    return err(TypesResolutionRegistryInferBindingError{
        .code = TypesResolutionRegistryInferBindingErrorCode::OUTSIDE_EXTENDS,
        .message = message,
    });
  }

  return ok(binding->resolved);
}

const Type *TypesResolutionRegistry::resolveInfer(const TypeRef &ref) {
  auto binding = this->inferBinding(ref);
  if (binding.isErr()) {
    this->state.diagnostics.error(ref.loc, binding.error().message);
    return Type::error();
  }

  return binding.value();
}

bool TypesResolutionRegistry::hasThisParam(const TypeRef &ref) const {
  size_t count = ref.args.size() - 1;
  bool isNamedThis =
      count > 0 && !ref.names().empty() && ref.names()[0] == KeywordTable::getName(Keyword::THIS);
  bool isOnlyRest = ref.rest() && count == 1;

  return isNamedThis && !ref.optional()[0] && !isOnlyRest;
}

TypesResolutionRegistry::ResultTupleLengthFixed
TypesResolutionRegistry::fixedLength(const Type *list) const {
  if (list->isTuple() && list->tupleRest() != nullptr &&
      !this->state.types.context.isInsideListContext() &&
      !this->state.types.context.isInsideExtendsClause()) {
    auto message = ErrorTextTypeShape::tupleRest();

    return err(TypesResolutionRegistryFixedLengthError{
        .code = TypesResolutionRegistryFixedLengthErrorCode::REST_ELEMENT,
        .message = message,
    });
  }

  return ok(list);
}

bool TypesResolutionRegistry::isSpreadWaiting(const TypeRef &ref,
                                              llvm::ArrayRef<const Type *> elements) const {
  for (size_t index = 0; index < elements.size(); ++index) {
    bool isOpen = elements[index]->typeOp() != TypeOp::None && !elements[index]->isListParam();
    if (ref.spread()[index] && isOpen) {
      return true;
    }
  }

  return false;
}

bool TypesResolutionRegistry::hasErrorType(llvm::ArrayRef<const Type *> types) const {
  for (const Type *type : types) {
    if (type->isError()) {
      return true;
    }
  }

  return false;
}

Overload &TypesResolutionRegistry::overloadNamed(std::vector<Overload> &overloads,
                                                 llvm::StringRef name) const {
  for (Overload &overload : overloads) {
    if (overload.name == name) {
      return overload;
    }
  }
  overloads.push_back(Overload{name.str(), {}});

  return overloads.back();
}

bool TypesResolutionRegistry::isMentioningAnyParam(llvm::ArrayRef<const Type *> operands) const {
  for (const Type *type : operands) {
    if (type != nullptr && TypeQueries::mentionsParam(type)) {
      return true;
    }
  }

  return false;
}

const Type *TypesResolutionRegistry::firstError(llvm::ArrayRef<const Type *> operands) const {
  for (const Type *type : operands) {
    if (type != nullptr && type->isError()) {
      return type;
    }
  }

  return nullptr;
}

const UtilityTypeName *TypesResolutionRegistry::findUtility(llvm::StringRef name) const {
  const auto *utility = TypeNameTable::findUtility(name);
  bool isShadowed = utility != nullptr && this->state.findTypeDeclaration(name) != nullptr;
  if (isShadowed) {
    return nullptr;
  }

  return utility;
}

TypesResolutionRegistry::ResultUtilityAllowed
TypesResolutionRegistry::checkArgumentCount(const TypeRef &ref,
                                            const UtilityTypeName &utility) const {
  if (int(ref.args.size()) != utility.argumentCount) {
    auto message = ErrorTextUtility::count(ref.name, utility.argumentCount);

    return err(TypesResolutionRegistryResolveUtilityTypeError{
        .code = TypesResolutionRegistryResolveUtilityTypeErrorCode::WRONG_ARGUMENT_COUNT,
        .message = message,
    });
  }

  return ok();
}

TypesResolutionRegistry::ResultUtilityAllowed
TypesResolutionRegistry::checkOperand(const TypeRef &ref, const UtilityTypeName &utility,
                                      const Type *operand) const {
  if (TypeQueries::mentionsParam(operand)) {
    return ok();
  }
  switch (utility.operand) {
    case UtilityOperand::ANY_TYPE:
      return ok();
    case UtilityOperand::FUNCTION_TYPE:
      return this->checkFunctionOperand(ref, operand);
    case UtilityOperand::TEXT_TYPE:
      return this->checkTextOperand(ref, operand);
  }

  return ok();
}

TypesResolutionRegistry::ResultUtilityAllowed
TypesResolutionRegistry::checkFunctionOperand(const TypeRef &ref, const Type *operand) const {
  if (!operand->nonOptional()->isFunction()) {
    auto message = ErrorTextUtility::noFunction(ref.name, operand->name());

    return err(TypesResolutionRegistryResolveUtilityTypeError{
        .code = TypesResolutionRegistryResolveUtilityTypeErrorCode::NOT_A_FUNCTION_TYPE,
        .message = message,
    });
  }

  return ok();
}

TypesResolutionRegistry::ResultUtilityAllowed
TypesResolutionRegistry::checkTextOperand(const TypeRef &ref, const Type *operand) const {
  const auto *nonString = this->firstNonString(operand);
  if (nonString != nullptr) {
    auto message = ErrorTextUtility::notString(ref.name, nonString->name());

    return err(TypesResolutionRegistryResolveUtilityTypeError{
        .code = TypesResolutionRegistryResolveUtilityTypeErrorCode::NOT_A_STRING_TYPE,
        .message = message,
    });
  }

  return ok();
}

const Type *TypesResolutionRegistry::firstNonString(const Type *operand) const {
  for (const Type *member : TypeopsCheckHelpers::distributedMembers(operand)) {
    if (!member->isString() && !member->isStringLiteral()) {
      return member;
    }
  }

  return nullptr;
}

void TypesResolutionRegistry::resolveTypeRef(const ResolveTypeRef &task) {
  TypeUse use = task.isVoidAllowed ? TypeUse::STANDALONE_OR_VOID : TypeUse::STANDALONE;
  this->state.walk.tasks.pushTask(WriteResolvedType{.ref = task.ref});
  this->state.walk.tasks.pushTask(ResolveTypeAnswer{.ref = task.ref, .use = use});
}

void TypesResolutionRegistry::resolveInnerTypeRef(const ResolveInnerTypeRef &task) {
  this->state.walk.tasks.pushTask(WriteResolvedType{.ref = task.ref});
  this->state.walk.tasks.pushTask(ResolveTypeAnswer{.ref = task.ref, .use = TypeUse::INNER});
}

void TypesResolutionRegistry::writeResolvedType(const WriteResolvedType &task) {
  task.ref->resolved = this->state.walk.tasks.takeAnswer();
}

void TypesResolutionRegistry::resolveTypeAnswer(const ResolveTypeAnswer &task) {
  if (task.use != TypeUse::INNER) {
    this->state.walk.tasks.pushTask(FinishTypeUse{.ref = task.ref, .use = task.use});
  }

  return this->startInnerType(*task.ref);
}

void TypesResolutionRegistry::finishTypeUse(const FinishTypeUse &task) {
  const TypeRef &ref = *task.ref;
  const Type *type = this->state.walk.tasks.takeAnswer();
  if (task.use == TypeUse::STANDALONE_OR_VOID) {
    const auto *standalone = this->standaloneType(ref, type);

    return this->state.walk.tasks.pushAnswer(standalone);
  }
  if (type->isVoid() && this->isBoundParam(ref)) {
    return this->state.walk.tasks.pushAnswer(Type::undefinedTy());
  }
  auto nonVoid = this->checkNotVoid(type);
  if (nonVoid.isErr()) {
    this->state.diagnostics.error(ref.loc, nonVoid.error().message);

    return this->state.walk.tasks.pushAnswer(Type::error());
  }
  const auto *standalone = this->standaloneType(ref, type);
  this->state.walk.tasks.pushAnswer(standalone);
}

void TypesResolutionRegistry::startInnerType(const TypeRef &ref) {
  switch (ref.kind()) {
    case TypeRefKind::NAME_REF:
      return this->startName(ref);
    case TypeRefKind::GENERIC_REF:
      return this->startGeneric(ref);
    case TypeRefKind::ARRAY_REF:
      return this->pushArray(ref);
    case TypeRefKind::UNION_REF:
      return this->startUnion(ref);
    case TypeRefKind::LITERAL_REF:
      return this->state.walk.tasks.pushAnswer(this->literalType(ref));
    case TypeRefKind::PREDICATE_REF:
      return this->state.walk.tasks.pushAnswer(this->rejectPredicate(ref));
    case TypeRefKind::KEY_OF_REF:
      return this->state.walk.tasks.pushTask(StartKeyOf{.ref = &ref});
    case TypeRefKind::INDEXED_REF:
      return this->state.walk.tasks.pushTask(StartIndexed{.ref = &ref});
    case TypeRefKind::TYPE_OF_REF:
      return this->state.walk.tasks.pushTask(StartTypeOf{.ref = &ref});
    case TypeRefKind::MAPPED_REF:
      return this->state.walk.tasks.pushTask(StartMapped{.ref = &ref});
    case TypeRefKind::CONDITIONAL_REF:
      return this->state.walk.tasks.pushTask(StartConditional{.ref = &ref, .isMember = false});
    case TypeRefKind::TEMPLATE_REF:
      return this->state.walk.tasks.pushTask(StartTemplate{.ref = &ref});
    case TypeRefKind::INTERSECTION_REF:
      return this->state.walk.tasks.pushTask(StartIntersection{.ref = &ref});
    case TypeRefKind::INFER_REF:
      return this->state.walk.tasks.pushAnswer(this->resolveInfer(ref));
    case TypeRefKind::FUNCTION_REF:
      return this->startFunctionType(ref);
    case TypeRefKind::OBJECT_REF:
      return this->startObject(ref);
    case TypeRefKind::TUPLE_REF:
      return this->startTuple(ref);
  }
  llvm_unreachable("unknown type reference");
}

void TypesResolutionRegistry::startName(const TypeRef &ref) {
  if (ref.name.empty()) {
    return this->state.walk.tasks.pushAnswer(Type::error());
  }
  const auto *type = Type::fromName(ref.name);
  if (type != nullptr) {
    return this->state.walk.tasks.pushAnswer(type);
  }
  auto *decl = this->state.findTypeDeclaration(ref.name);
  if (TypeNameTable::isName(ref.name, TypeName::NEVER) && decl == nullptr) {
    return this->state.walk.tasks.pushAnswer(Type::never());
  }
  if (decl != nullptr) {
    return this->startDeclared(ref, decl);
  }
  auto [owner, member] = ref.name.split('.');
  EnumDecl *declaredEnum = this->enumOwning(owner, member);
  if (declaredEnum != nullptr) {
    this->state.walk.tasks.pushTask(FinishEnumMember{.ref = &ref, .enumDecl = declaredEnum});

    return this->state.walk.tasks.pushTask(ResolveEnum{.enumDecl = declaredEnum});
  }
  if (ref.name.starts_with(kIntlPrefix)) {
    return this->startIntl(ref);
  }

  return this->startOtherName(ref);
}

void TypesResolutionRegistry::startOtherName(const TypeRef &ref) {
  const auto *builtin = this->builtinNamed(ref.name);
  if (builtin != nullptr) {
    return this->state.walk.tasks.pushAnswer(builtin);
  }
  if (this->isDictName(ref.name)) {
    return this->state.walk.tasks.pushTask(ResolveDictType{.ref = &ref});
  }
  if (TypeNameTable::isName(ref.name, TypeName::CONSTRUCTOR)) {
    const auto *object = Type::anyObject();
    const auto *constructor = Type::constructorOf(object);

    return this->state.walk.tasks.pushAnswer(constructor);
  }
  if (TypeNameTable::isName(ref.name, TypeName::CONSTRUCTOR_ARGS)) {
    return this->state.walk.tasks.pushTask(CheckConstructorArgs{.ref = &ref});
  }
  const auto *missing = this->missingType(ref);
  this->state.walk.tasks.pushAnswer(missing);
}

void TypesResolutionRegistry::startDeclared(const TypeRef &ref, Stmt *decl) {
  auto *alias = dyn_cast<TypeAliasDecl>(decl);
  auto *classDecl = dyn_cast<ClassDecl>(decl);
  if (alias != nullptr && alias->typeParams.empty()) {
    this->state.walk.tasks.pushTask(FinishAliasType{.alias = alias});

    return this->state.walk.tasks.pushTask(ResolveAlias{.alias = alias});
  }
  if (alias != nullptr) {
    return this->state.walk.tasks.pushTask(
        InstantiateAlias{.alias = alias, .args = {}, .loc = ref.loc});
  }
  if (classDecl != nullptr && classDecl->isGeneric()) {
    return this->state.walk.tasks.pushTask(
        InstantiateClassType{.classDecl = classDecl, .args = {}, .loc = ref.loc});
  }
  if (classDecl != nullptr) {
    const ClassDecl *finalDecl = classDecl->findFinalClass();

    return this->state.walk.tasks.pushAnswer(finalDecl->type);
  }
  auto *enumDecl = cast<EnumDecl>(decl);
  this->state.walk.tasks.pushTask(FinishEnumType{.enumDecl = enumDecl});
  this->state.walk.tasks.pushTask(ResolveEnum{.enumDecl = enumDecl});
}

void TypesResolutionRegistry::finishAliasType(const FinishAliasType &task) {
  const TypeAliasDecl *alias = task.alias;
  const Type *type = alias->resolved != nullptr ? alias->resolved : alias->self;
  this->state.walk.tasks.pushAnswer(type);
}

void TypesResolutionRegistry::finishEnumType(const FinishEnumType &task) {
  const auto *values = task.enumDecl->valuesType();
  this->state.walk.tasks.pushAnswer(values);
}

void TypesResolutionRegistry::finishEnumMember(const FinishEnumMember &task) {
  const TypeRef &ref = *task.ref;
  auto [owner, member] = ref.name.split('.');
  auto found = this->enumMember(task.enumDecl, owner, member);
  if (found.isErr()) {
    this->state.diagnostics.error(ref.loc, found.error().message);

    return this->state.walk.tasks.pushAnswer(Type::error());
  }
  const EnumMember *value = found.value();
  if (value->isString) {
    return this->state.walk.tasks.pushAnswer(Type::stringLiteral(value->text));
  }
  const auto *number = Type::numberLiteral(double(value->number), Type::intTy());
  this->state.walk.tasks.pushAnswer(number);
}

void TypesResolutionRegistry::startIntl(const TypeRef &ref) {
  ClassDecl *locale = this->localeClass(ref.name);
  if (locale != nullptr) {
    const ClassDecl *finalDecl = locale->findFinalClass();

    return this->state.walk.tasks.pushAnswer(finalDecl->type);
  }
  auto shortName = ref.name.drop_front(kIntlPrefix.size());
  this->state.walk.tasks.pushTask(FinishIntlType{.ref = &ref});
  this->state.walk.tasks.pushTask(FindIntlType{.name = shortName});
}

void TypesResolutionRegistry::finishIntlType(const FinishIntlType &task) {
  const TypeRef &ref = *task.ref;
  const Type *intl = this->state.walk.tasks.takeAnswer();
  if (intl == nullptr) {
    auto message = ErrorTextTypeName::notFound(ref.name);
    this->state.diagnostics.error(ref.loc, message);

    return this->state.walk.tasks.pushAnswer(Type::error());
  }
  this->state.walk.tasks.pushAnswer(intl);
}

void TypesResolutionRegistry::startGeneric(const TypeRef &ref) {
  auto *decl = this->state.findTypeDeclaration(ref.name);
  auto *classDecl = dyn_cast_or_null<ClassDecl>(decl);
  auto *alias = dyn_cast_or_null<TypeAliasDecl>(decl);
  const auto *bytes = this->bytesTypeNamed(ref.name);
  bool isTypedArray = bytes != nullptr && bytes->isTypedArray() && decl == nullptr;
  if (isTypedArray) {
    const auto *typed = this->resolveTypedArray(ref, bytes);

    return this->state.walk.tasks.pushAnswer(typed);
  }
  auto result = this->resultTypeNamed(ref.name);
  if (result.has_value()) {
    return this->state.walk.tasks.pushTask(ResolveResultType{.ref = &ref, .typeName = *result});
  }
  const auto *builtin = TypeNameTable::find(ref.name);
  bool isArray = builtin != nullptr && builtin->form.is<ArrayTypeName>();
  if (isArray) {
    return this->startArrayGeneric(ref);
  }
  if (classDecl != nullptr) {
    return this->startTypeArguments(ref, classDecl, TypeArgumentsUse::CLASS);
  }
  if (alias != nullptr) {
    return this->startTypeArguments(ref, alias, TypeArgumentsUse::ALIAS);
  }

  return this->startLibraryGeneric(ref);
}

void TypesResolutionRegistry::startLibraryGeneric(const TypeRef &ref) {
  if (this->isIteratorName(ref.name)) {
    return this->state.walk.tasks.pushTask(ResolveIteratorType{.ref = &ref});
  }
  if (this->isDictName(ref.name)) {
    return this->state.walk.tasks.pushTask(ResolveDictType{.ref = &ref});
  }
  if (this->isPromiseName(ref.name)) {
    return this->state.walk.tasks.pushTask(ResolvePromiseType{.ref = &ref});
  }
  if (TypeNameTable::isName(ref.name, TypeName::METHOD_DESCRIPTOR)) {
    return this->startDescriptor(ref);
  }
  const auto *utility = this->findUtility(ref.name);
  if (utility != nullptr) {
    return this->startUtility(ref, utility);
  }
  if (TypeNameTable::isName(ref.name, TypeName::CONSTRUCTOR)) {
    return this->state.walk.tasks.pushTask(ResolveConstructorType{.ref = &ref});
  }
  if (TypeNameTable::isName(ref.name, TypeName::INSTANCE_TYPE) && ref.args.size() == 1) {
    this->state.walk.tasks.pushTask(FinishInstanceType{.ref = &ref});

    return this->state.walk.tasks.pushTask(
        ResolveTypeAnswer{.ref = &ref.args[0], .use = TypeUse::STANDALONE});
  }
  auto message = ErrorTextGenericRef::noSupport(ref.name);
  this->state.diagnostics.error(ref.loc, message);
  this->state.walk.tasks.pushAnswer(Type::error());
}

void TypesResolutionRegistry::startArrayGeneric(const TypeRef &ref) {
  auto given = this->arrayArgument(ref);
  if (given.isErr()) {
    this->state.diagnostics.error(ref.loc, given.error().message);

    return this->state.walk.tasks.pushAnswer(Type::error());
  }

  return this->pushArray(ref);
}

void TypesResolutionRegistry::pushArray(const TypeRef &ref) {
  this->state.walk.tasks.pushTask(FinishArrayType{.ref = &ref});
  this->state.walk.tasks.pushTask(ResolveTypeAnswer{.ref = &ref.args[0], .use = TypeUse::INNER});
}

void TypesResolutionRegistry::finishArrayType(const FinishArrayType &task) {
  const Type *element = this->state.walk.tasks.takeAnswer();
  auto allowed = this->arrayElement(element);
  if (allowed.isErr()) {
    this->state.diagnostics.error(task.ref->loc, allowed.error().message);

    return this->state.walk.tasks.pushAnswer(Type::error());
  }
  const auto *array = Type::array(element);
  this->state.walk.tasks.pushAnswer(array);
}

void TypesResolutionRegistry::startTypeArguments(const TypeRef &ref, Stmt *decl,
                                                 TypeArgumentsUse use) {
  auto draft = TypeArgumentsDraft{
      .ref = &ref,
      .decl = decl,
      .use = use,
      .next = 0,
      .args = {},
  };

  return this->nextTypeArgument(NextTypeArgument{.draft = draft});
}

void TypesResolutionRegistry::nextTypeArgument(const NextTypeArgument &task) {
  const TypeArgumentsDraft &draft = task.draft;
  const TypeRef &ref = *draft.ref;
  if (draft.next == ref.args.size()) {
    return this->finishTypeArguments(draft);
  }
  TypeUse use = draft.use == TypeArgumentsUse::CLASS ? TypeUse::STANDALONE_OR_VOID : TypeUse::INNER;
  this->state.walk.tasks.pushTask(AfterTypeArgument{.draft = draft});
  this->state.walk.tasks.pushTask(ResolveTypeAnswer{.ref = &ref.args[draft.next], .use = use});
}

void TypesResolutionRegistry::afterTypeArgument(const AfterTypeArgument &task) {
  TypeArgumentsDraft draft = task.draft;
  const Type *type = this->state.walk.tasks.takeAnswer();
  if (type->isError()) {
    return this->state.walk.tasks.pushAnswer(type);
  }
  draft.args.push_back(type);
  draft.next += 1;

  return this->nextTypeArgument(NextTypeArgument{.draft = draft});
}

void TypesResolutionRegistry::finishTypeArguments(const TypeArgumentsDraft &draft) {
  const TypeRef &ref = *draft.ref;
  auto *classDecl = dyn_cast<ClassDecl>(draft.decl);
  auto *alias = dyn_cast<TypeAliasDecl>(draft.decl);
  if (draft.use == TypeArgumentsUse::TAIL_CALL) {
    this->state.types.context.rememberTailCall(alias, draft.args);

    return this->state.walk.tasks.pushAnswer(nullptr);
  }
  if (draft.use == TypeArgumentsUse::CLASS) {
    auto generic = this->classGeneric(classDecl, ref.name);
    if (generic.isErr()) {
      this->state.diagnostics.error(ref.loc, generic.error().message);

      return this->state.walk.tasks.pushAnswer(Type::error());
    }

    return this->state.walk.tasks.pushTask(
        InstantiateClassType{.classDecl = classDecl, .args = draft.args, .loc = ref.loc});
  }
  auto generic = this->aliasGeneric(alias, ref.name);
  if (generic.isErr()) {
    this->state.diagnostics.error(ref.loc, generic.error().message);

    return this->state.walk.tasks.pushAnswer(Type::error());
  }
  this->state.walk.tasks.pushTask(
      InstantiateAlias{.alias = alias, .args = draft.args, .loc = ref.loc});
}

void TypesResolutionRegistry::startDescriptor(const TypeRef &ref) {
  auto arity = this->descriptorArity(ref);
  if (arity.isErr()) {
    this->state.diagnostics.error(ref.loc, arity.error().message);

    return this->state.walk.tasks.pushAnswer(Type::error());
  }
  this->state.walk.tasks.pushTask(FinishDescriptor{.ref = &ref});
  this->state.walk.tasks.pushTask(
      ResolveTypeAnswer{.ref = &ref.args[1], .use = TypeUse::STANDALONE_OR_VOID});
  this->state.walk.tasks.pushTask(
      ResolveTypeAnswer{.ref = &ref.args[0], .use = TypeUse::STANDALONE});
}

void TypesResolutionRegistry::finishDescriptor(const FinishDescriptor &task) {
  const TypeRef &ref = *task.ref;
  const Type *returned = this->state.walk.tasks.takeAnswer();
  const Type *params = this->state.walk.tasks.takeAnswer();
  if (params->isError() || returned->isError()) {
    return this->state.walk.tasks.pushAnswer(Type::error());
  }
  auto list = this->descriptorParams(params);
  if (list.isErr()) {
    this->state.diagnostics.error(ref.args[0].loc, list.error().message);

    return this->state.walk.tasks.pushAnswer(Type::error());
  }
  this->state.walk.tasks.pushTask(MakeMethodDescriptor{.params = params, .returned = returned});
}

void TypesResolutionRegistry::finishInstanceType(const FinishInstanceType &task) {
  const TypeRef &ref = *task.ref;
  const Type *classType = this->state.walk.tasks.takeAnswer();
  if (classType->isError()) {
    return this->state.walk.tasks.pushAnswer(classType);
  }
  auto made = this->classInstance(classType);
  if (made.isErr()) {
    this->state.diagnostics.error(ref.args[0].loc, made.error().message);

    return this->state.walk.tasks.pushAnswer(Type::error());
  }
  this->state.walk.tasks.pushAnswer(made.value());
}

void TypesResolutionRegistry::startUtility(const TypeRef &ref, const UtilityTypeName *utility) {
  auto argumentCount = this->checkArgumentCount(ref, *utility);
  if (argumentCount.isErr()) {
    this->state.diagnostics.error(ref.loc, argumentCount.error().message);

    return this->state.walk.tasks.pushAnswer(Type::error());
  }
  auto draft = UtilityDraft{
      .ref = &ref,
      .utility = utility,
      .next = 0,
      .operands = {},
  };

  return this->nextUtilityOperand(NextUtilityOperand{.draft = draft});
}

void TypesResolutionRegistry::nextUtilityOperand(const NextUtilityOperand &task) {
  const UtilityDraft &draft = task.draft;
  const TypeRef &arg = draft.ref->args[draft.next];
  if (draft.utility->operand == UtilityOperand::FUNCTION_TYPE) {
    this->state.walk.tasks.pushTask(AfterUtilitySignature{.draft = draft});

    return this->state.walk.tasks.pushTask(SignatureForUtility{.ref = &arg});
  }
  this->state.walk.tasks.pushTask(AfterUtilityOperand{.draft = draft});
  this->state.walk.tasks.pushTask(ResolveTypeAnswer{.ref = &arg, .use = TypeUse::INNER});
}

void TypesResolutionRegistry::afterUtilitySignature(const AfterUtilitySignature &task) {
  const UtilityDraft &draft = task.draft;
  const Type *signature = this->state.walk.tasks.takeAnswer();
  if (signature != nullptr) {
    return this->addUtilityOperand(draft, signature);
  }
  const TypeRef &arg = draft.ref->args[draft.next];
  this->state.walk.tasks.pushTask(AfterUtilityOperand{.draft = draft});
  this->state.walk.tasks.pushTask(ResolveTypeAnswer{.ref = &arg, .use = TypeUse::INNER});
}

void TypesResolutionRegistry::afterUtilityOperand(const AfterUtilityOperand &task) {
  const Type *operand = this->state.walk.tasks.takeAnswer();

  return this->addUtilityOperand(task.draft, operand);
}

void TypesResolutionRegistry::addUtilityOperand(UtilityDraft draft, const Type *operand) {
  draft.operands.push_back(operand);
  draft.next += 1;
  if (operand->isError() || draft.next == draft.ref->args.size()) {
    return this->finishUtility(draft);
  }

  return this->nextUtilityOperand(NextUtilityOperand{.draft = draft});
}

void TypesResolutionRegistry::finishUtility(const UtilityDraft &draft) {
  const TypeRef &ref = *draft.ref;
  const std::vector<const Type *> &operands = draft.operands;
  if (operands.back()->isError()) {
    return this->state.walk.tasks.pushAnswer(operands.back());
  }
  auto operand = this->checkOperand(ref, *draft.utility, operands.front());
  if (operand.isErr()) {
    this->state.diagnostics.error(ref.loc, operand.error().message);

    return this->state.walk.tasks.pushAnswer(Type::error());
  }
  const auto *unevaluated = Type::typeOp(draft.utility->op, operands);
  this->state.walk.tasks.pushTask(EvalTypeOp{.op = unevaluated, .operands = operands});
}

void TypesResolutionRegistry::startUnion(const TypeRef &ref) {
  auto draft = UnionDraft{
      .ref = &ref,
      .next = 0,
      .isWithNull = false,
      .members = {},
  };

  return this->nextUnionMember(NextUnionMember{.draft = draft});
}

void TypesResolutionRegistry::nextUnionMember(const NextUnionMember &task) {
  UnionDraft draft = task.draft;
  const TypeRef &ref = *draft.ref;
  while (draft.next < ref.args.size() && this->isNullRef(ref.args[draft.next])) {
    draft.isWithNull = true;
    draft.next += 1;
  }
  if (draft.next == ref.args.size()) {
    return this->finishUnion(draft);
  }
  this->state.walk.tasks.pushTask(AfterUnionMember{.draft = draft});
  this->state.walk.tasks.pushTask(
      ResolveTypeAnswer{.ref = &ref.args[draft.next], .use = TypeUse::INNER});
}

void TypesResolutionRegistry::afterUnionMember(const AfterUnionMember &task) {
  UnionDraft draft = task.draft;
  const Type *type = this->state.walk.tasks.takeAnswer();
  if (type->isError()) {
    return this->state.walk.tasks.pushAnswer(type);
  }
  auto part = this->unionPart(type);
  if (part.isErr()) {
    this->state.diagnostics.error(draft.ref->loc, part.error().message);

    return this->state.walk.tasks.pushAnswer(Type::error());
  }
  draft.members.push_back(type);
  draft.next += 1;

  return this->nextUnionMember(NextUnionMember{.draft = draft});
}

void TypesResolutionRegistry::finishUnion(const UnionDraft &draft) {
  const auto *type = Type::unionOf(draft.members);
  auto supported = this->unionAllowed(type, draft.isWithNull);
  if (supported.isErr()) {
    this->state.diagnostics.error(draft.ref->loc, supported.error().message);

    return this->state.walk.tasks.pushAnswer(Type::error());
  }
  this->state.walk.tasks.pushAnswer(type);
}

void TypesResolutionRegistry::startFunctionType(const TypeRef &ref) {
  auto draft = FunctionTypeDraft{
      .ref = &ref,
      .next = 0,
      .types = {},
      .minArgs = 0,
      .hasOptional = false,
      .isValid = true,
  };

  return this->nextFunctionParam(NextFunctionParam{.draft = draft});
}

void TypesResolutionRegistry::nextFunctionParam(const NextFunctionParam &task) {
  const FunctionTypeDraft &draft = task.draft;
  const TypeRef &ref = *draft.ref;
  size_t count = ref.args.size() - 1;
  if (draft.next == count) {
    this->state.walk.tasks.pushTask(FinishFunctionType{.draft = draft});

    return this->startReturned(ref.args.back());
  }
  bool isRest = ref.rest() && draft.next + 1 == count;
  this->state.types.context.enterListContext(isRest);
  this->state.walk.tasks.pushTask(AfterFunctionParam{.draft = draft});
  this->state.walk.tasks.pushTask(
      ResolveTypeAnswer{.ref = &ref.args[draft.next], .use = TypeUse::STANDALONE});
}

void TypesResolutionRegistry::afterFunctionParam(const AfterFunctionParam &task) {
  this->state.types.context.leaveListContext();
  FunctionTypeDraft draft = task.draft;
  const TypeRef &ref = *draft.ref;
  const TypeRef &param = ref.args[draft.next];
  const Type *type = this->state.walk.tasks.takeAnswer();
  size_t count = ref.args.size() - 1;
  bool isRest = ref.rest() && draft.next + 1 == count;
  draft.isValid = draft.isValid && !type->isError();
  if (isRest) {
    this->addRestParam(draft, param, type);
  } else if (ref.optional()[draft.next]) {
    this->addOptionalParam(draft, type);
  } else {
    this->addRequiredParam(draft, param, type);
  }
  draft.next += 1;

  return this->nextFunctionParam(NextFunctionParam{.draft = draft});
}

void TypesResolutionRegistry::startReturned(const TypeRef &returned) {
  if (returned.kind() != TypeRefKind::PREDICATE_REF) {
    return this->state.walk.tasks.pushTask(
        ResolveTypeAnswer{.ref = &returned, .use = TypeUse::STANDALONE_OR_VOID});
  }
  if (!returned.asserts()) {
    return this->state.walk.tasks.pushAnswer(Type::boolTy());
  }
  if (returned.args.empty()) {
    this->reportAsserts(returned, nullptr);

    return this->state.walk.tasks.pushAnswer(Type::error());
  }
  this->state.walk.tasks.pushTask(FinishAssertsType{.returned = &returned});
  this->state.walk.tasks.pushTask(
      ResolveTypeAnswer{.ref = &returned.args[0], .use = TypeUse::STANDALONE});
}

void TypesResolutionRegistry::finishAssertsType(const FinishAssertsType &task) {
  const Type *type = this->state.walk.tasks.takeAnswer();
  this->reportAsserts(*task.returned, type);
  this->state.walk.tasks.pushAnswer(Type::error());
}

void TypesResolutionRegistry::reportAsserts(const TypeRef &returned, const Type *type) {
  auto shown = returned.name.str();
  bool hasType = type != nullptr;
  if (hasType) {
    shown += " is " + type->name().str();
  }
  auto message = ErrorTextAsserts::notAllowed(shown, hasType, "");
  this->state.diagnostics.error(returned.loc, message);
}

void TypesResolutionRegistry::finishFunctionType(const FinishFunctionType &task) {
  const FunctionTypeDraft &draft = task.draft;
  const TypeRef &ref = *draft.ref;
  const Type *returnType = this->state.walk.tasks.takeAnswer();
  if (!draft.isValid || returnType->isError()) {
    return this->state.walk.tasks.pushAnswer(Type::error());
  }
  bool hasThis = this->hasThisParam(ref);
  const auto *function =
      Type::function(draft.types, draft.minArgs, ref.rest(), returnType, hasThis);
  this->state.walk.tasks.pushAnswer(function);
}

void TypesResolutionRegistry::startTuple(const TypeRef &ref) {
  this->state.walk.tasks.pushTask(FinishTupleType{.ref = &ref});
  for (const TypeRef &element : llvm::reverse(ref.args)) {
    this->state.walk.tasks.pushTask(ResolveTypeAnswer{.ref = &element, .use = TypeUse::STANDALONE});
  }
}

void TypesResolutionRegistry::finishTupleType(const FinishTupleType &task) {
  const TypeRef &ref = *task.ref;
  auto elements = std::vector<const Type *>(ref.args.size(), nullptr);
  for (size_t index = ref.args.size(); index > 0; --index) {
    elements[index - 1] = this->state.walk.tasks.takeAnswer();
  }
  if (llvm::is_contained(ref.spread(), true)) {
    return this->finishSpreadTuple(ref, elements);
  }
  for (size_t index = 0; index < elements.size(); ++index) {
    auto element = this->tupleElement(elements[index]);
    if (element.isErr()) {
      this->state.diagnostics.error(ref.args[index].loc, element.error().message);

      return this->state.walk.tasks.pushAnswer(Type::error());
    }
  }
  bool isReadonly = !ref.readonly().empty() && ref.readonly()[0];
  const auto *tuple = Type::tuple(elements, isReadonly);
  this->state.walk.tasks.pushAnswer(tuple);
}

void TypesResolutionRegistry::finishSpreadTuple(const TypeRef &ref,
                                                const std::vector<const Type *> &elements) {
  if (this->isSpreadWaiting(ref, elements) && !this->hasErrorType(elements)) {
    return this->pushDeferredSpread(ref, elements);
  }
  auto joined = this->spreadJoined(elements, ref.spread());
  if (joined.isErr()) {
    this->state.diagnostics.error(ref.loc, joined.error().message);

    return this->state.walk.tasks.pushAnswer(Type::error());
  }
  const Type *list = joined.value();
  auto fixed = this->fixedLength(list);
  if (fixed.isErr()) {
    this->state.diagnostics.error(ref.loc, fixed.error().message);

    return this->state.walk.tasks.pushAnswer(Type::error());
  }
  this->state.walk.tasks.pushAnswer(list);
}

void TypesResolutionRegistry::pushDeferredSpread(const TypeRef &ref,
                                                 llvm::ArrayRef<const Type *> elements) {
  auto operands = std::vector<const Type *>();
  for (size_t index = 0; index < elements.size(); ++index) {
    operands.push_back(Type::boolLiteral(ref.spread()[index]));
    operands.push_back(elements[index]);
  }
  const auto *spread = Type::typeOp(TypeOp::Spread, operands);
  this->state.walk.tasks.pushTask(EvalTypeOp{.op = spread, .operands = operands});
}

void TypesResolutionRegistry::startObject(const TypeRef &ref) {
  auto draft = ObjectDraft{
      .ref = &ref,
      .target = nullptr,
      .next = 0,
      .fields = {},
      .overloads = {},
  };

  return this->nextObjectField(NextObjectField{.draft = draft});
}

void TypesResolutionRegistry::resolveObjectFields(const ResolveObjectFields &task) {
  auto draft = ObjectDraft{
      .ref = task.ref,
      .target = task.ref,
      .next = 0,
      .fields = {},
      .overloads = {},
  };

  return this->nextObjectField(NextObjectField{.draft = draft});
}

void TypesResolutionRegistry::nextObjectField(const NextObjectField &task) {
  ObjectDraft draft = task.draft;
  const TypeRef &ref = *draft.ref;
  while (draft.next < ref.args.size()) {
    bool isSignature = this->isOverloaded(ref, draft.next);
    auto problem = isSignature ? this->signatureProblem(draft) : this->fieldProblem(draft);
    if (!problem.has_value()) {
      return this->pushObjectMember(draft, isSignature);
    }
    this->state.diagnostics.error(ref.locs()[draft.next], *problem);
    draft.next += 1;
  }

  return this->finishObject(draft);
}

std::optional<std::string>
TypesResolutionRegistry::signatureProblem(const ObjectDraft &draft) const {
  llvm::StringRef name = draft.ref->names()[draft.next];
  const auto *overloads = draft.target != nullptr ? &draft.overloads : nullptr;
  auto allowed = this->overloadAllowed(overloads, name);
  if (allowed.isErr()) {
    return allowed.error().message;
  }

  return std::nullopt;
}

std::optional<std::string> TypesResolutionRegistry::fieldProblem(const ObjectDraft &draft) const {
  llvm::StringRef name = draft.ref->names()[draft.next];
  auto unique = this->newField(draft.fields, name);
  if (unique.isErr()) {
    return unique.error().message;
  }

  return std::nullopt;
}

void TypesResolutionRegistry::pushObjectMember(const ObjectDraft &draft, bool isSignature) {
  const TypeRef &member = draft.ref->args[draft.next];
  if (isSignature) {
    this->state.walk.tasks.pushTask(AfterObjectSignature{.draft = draft});
  } else {
    this->state.walk.tasks.pushTask(AfterObjectField{.draft = draft});
  }
  this->state.walk.tasks.pushTask(ResolveTypeAnswer{.ref = &member, .use = TypeUse::STANDALONE});
}

void TypesResolutionRegistry::afterObjectSignature(const AfterObjectSignature &task) {
  ObjectDraft draft = task.draft;
  const TypeRef &ref = *draft.ref;
  llvm::StringRef name = ref.names()[draft.next];
  const Type *type = this->state.walk.tasks.takeAnswer();
  Overload &overload = this->overloadNamed(draft.overloads, name);
  auto unique = this->newSignature(overload, type, name);
  if (unique.isErr()) {
    this->state.diagnostics.error(ref.locs()[draft.next], unique.error().message);
  } else if (!type->isError()) {
    overload.signatures.push_back(type);
  }
  draft.next += 1;

  return this->nextObjectField(NextObjectField{.draft = draft});
}

void TypesResolutionRegistry::afterObjectField(const AfterObjectField &task) {
  ObjectDraft draft = task.draft;
  const TypeRef &ref = *draft.ref;
  llvm::StringRef name = ref.names()[draft.next];
  const Type *written = this->state.walk.tasks.takeAnswer();
  const Type *type = ref.optional()[draft.next] ? Type::optional(written) : written;
  draft.fields.push_back({name.str(), type, ref.readonly()[draft.next]});
  draft.next += 1;

  return this->nextObjectField(NextObjectField{.draft = draft});
}

void TypesResolutionRegistry::finishObject(const ObjectDraft &draft) {
  if (draft.target == nullptr) {
    const auto *object = Type::object(draft.fields);

    return this->state.walk.tasks.pushAnswer(object);
  }
  ObjectTypeRef *shape = draft.target->form.as<ObjectTypeRef>();
  shape->resolvedFields = draft.fields;
  shape->resolvedOverloads = draft.overloads;
}

void TypesResolutionRegistry::evalTypeOp(const EvalTypeOp &task) {
  TypeOp kind = task.op->typeOp();
  if (kind == TypeOp::Conditional) {
    return this->state.walk.tasks.pushTask(EvalConditional{.operands = task.operands});
  }
  if (kind == TypeOp::Mapped) {
    return this->state.walk.tasks.pushTask(EvalMapped{.op = task.op, .operands = task.operands});
  }
  if (this->isMentioningAnyParam(task.operands)) {
    const auto *open = Type::typeOp(kind, task.operands);

    return this->state.walk.tasks.pushAnswer(open);
  }
  const auto *failed = this->firstError(task.operands);
  if (failed != nullptr) {
    return this->state.walk.tasks.pushAnswer(failed);
  }

  return this->pushReadyTypeOp(kind, task.operands);
}

void TypesResolutionRegistry::pushReadyTypeOp(TypeOp kind,
                                              const std::vector<const Type *> &operands) {
  switch (kind) {
    case TypeOp::KeyOf:
      return this->state.walk.tasks.pushTask(EvalKeyOf{.operand = operands[0]});
    case TypeOp::Index:
      return this->state.walk.tasks.pushTask(
          EvalIndexed{.object = operands[0], .key = operands[1]});
    case TypeOp::Exclude:
    case TypeOp::Extract:
      return this->state.walk.tasks.pushTask(
          EvalFiltered{.kind = kind, .members = operands[0], .filter = operands[1]});
    case TypeOp::Template:
      return this->state.walk.tasks.pushTask(EvalTemplate{.operands = operands});
    case TypeOp::Spread:
      return this->state.walk.tasks.pushTask(EvalSpread{.operands = operands});
    case TypeOp::Uppercase:
    case TypeOp::Lowercase:
    case TypeOp::Capitalize:
    case TypeOp::Uncapitalize:
      return this->state.walk.tasks.pushTask(EvalStringCase{.kind = kind, .operand = operands[0]});
    case TypeOp::NonNullable:
      return this->state.walk.tasks.pushAnswer(operands[0]->nonOptional());
    case TypeOp::ReturnType:
    case TypeOp::Parameters:
      return this->state.walk.tasks.pushTask(
          EvalFunctionPart{.kind = kind, .operand = operands[0]});
    case TypeOp::None:
    case TypeOp::Mapped:
    case TypeOp::Conditional:
      return this->state.walk.tasks.pushAnswer(Type::error());
  }
  this->state.walk.tasks.pushAnswer(Type::error());
}

void TypesResolutionRegistry::addRestParam(FunctionTypeDraft &draft, const TypeRef &param,
                                           const Type *type) {
  auto list = this->restParam(type);
  if (list.isErr()) {
    this->state.diagnostics.error(param.loc, list.error().message);
    draft.isValid = false;
  }
  draft.types.push_back(type);
}

void TypesResolutionRegistry::addOptionalParam(FunctionTypeDraft &draft, const Type *type) const {
  draft.hasOptional = true;
  draft.types.push_back(Type::optional(type));
}

void TypesResolutionRegistry::addRequiredParam(FunctionTypeDraft &draft, const TypeRef &param,
                                               const Type *type) {
  auto order = this->paramOrder(draft.hasOptional);
  if (order.isErr()) {
    this->state.diagnostics.error(param.loc, order.error().message);
    draft.isValid = false;
  }
  draft.types.push_back(type);
  draft.minArgs = draft.types.size();
}

const Type *TypesResolutionRegistry::bytesTypeNamed(llvm::StringRef name) const {
  for (Type::Bytes kind : kNamedBytes) {
    const Type *type = Type::bytesType(kind);
    if (type->name() == name) {
      return type;
    }
  }

  return nullptr;
}

const Type *TypesResolutionRegistry::cryptoTypeNamed(llvm::StringRef name) const {
  const auto *nodeName = this->state.findNodeName(name);
  if (nodeName == nullptr || nodeName->module != NodeModule::Crypto) {
    return nullptr;
  }
  auto kind = CryptoClassTable::find(nodeName->name);
  if (!kind.has_value()) {
    return nullptr;
  }

  return Type::cryptoType(*kind);
}

std::optional<TypeName> TypesResolutionRegistry::resultTypeNamed(llvm::StringRef name) const {
  if (this->state.findTypeDeclaration(name) != nullptr) {
    return std::nullopt;
  }
  const auto *builtin = TypeNameTable::find(name);
  if (builtin == nullptr || !builtin->form.is<ResultTypeName>()) {
    return std::nullopt;
  }

  return builtin->type;
}

bool TypesResolutionRegistry::isIteratorName(llvm::StringRef name) const {
  bool isUnshadowed = this->state.findTypeDeclaration(name) == nullptr;
  bool isIteratorResult = TypeNameTable::isName(name, TypeName::ITERATOR_RESULT);
  bool isIterable = TypeNameTable::isName(name, TypeName::ITERABLE);
  if ((isIteratorResult || isIterable) && isUnshadowed) {
    return true;
  }
  const auto *builtin = TypeNameTable::find(name);
  bool isIteratorForm = builtin != nullptr && builtin->form.is<IteratorTypeName>();
  bool isShadowed = TypeNameTable::isName(name, TypeName::ITERATOR) && !isUnshadowed;

  return isIteratorForm && !isShadowed;
}

bool TypesResolutionRegistry::isDictName(llvm::StringRef name) const {
  if (TypeNameTable::isName(name, TypeName::WEAK_REF)) {
    return true;
  }
  const auto *builtin = TypeNameTable::find(name);
  if (builtin == nullptr) {
    return false;
  }

  return builtin->form.is<WeakCollectionTypeName>() || builtin->form.is<CollectionTypeName>();
}

bool TypesResolutionRegistry::isPromiseName(llvm::StringRef name) const {
  const auto *builtin = TypeNameTable::find(name);

  return builtin != nullptr && builtin->form.is<PromiseTypeName>();
}

const Type *TypesResolutionRegistry::constructedBy(const Type *type) const {
  if (type->isConstructor()) {
    return type->constructed();
  }
  const ClassDecl *classDecl = type->classRef();

  return classDecl != nullptr ? classDecl->type : nullptr;
}

}
