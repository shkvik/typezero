#include "compiler/semantic/bodies/expr/member_checker/member_checker.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/number/number_static_table/number_static_table.h"
#include "compiler/semantic/bodies/classes/class_member_checker/class_member_checker.h"
#include "compiler/semantic/bodies/classes/overload_resolver/overload_resolver.h"
#include "compiler/semantic/bodies/classes/static_checker/static_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/objects/dict_checker/dict_checker.h"
#include "compiler/semantic/bodies/objects/keyed_field_checker/keyed_field_checker.h"
#include "compiler/semantic/bodies/objects/object_checker/object_checker.h"
#include "compiler/semantic/bodies/objects/tuple_checker/tuple_checker.h"
#include "compiler/semantic/declare/constant_folder/constant_folder.h"
#include "compiler/semantic/declare/type_declarer/type_declarer.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/flow/index_proofs/index_proofs.h"
#include "compiler/semantic/generics/arg_list_checker/arg_list_checker.h"
#include "compiler/semantic/generics/generic_checker/generic_checker.h"
#include "compiler/semantic/library/builtin_modules/builtin_module_checker/builtin_module_checker.h"
#include "compiler/semantic/library/builtin_modules/runtime_object_checker/runtime_object_checker.h"
#include "compiler/semantic/library/bytes/bytes_checker/bytes_checker.h"
#include "compiler/semantic/library/crypto/crypto_checker/crypto_checker.h"
#include "compiler/semantic/library/date/date_checker/date_checker.h"
#include "compiler/semantic/library/intl/intl_checker/intl_checker.h"
#include "compiler/semantic/library/json/json_checker/json_checker.h"
#include "compiler/semantic/library/process/process_checker/process_checker.h"
#include "compiler/semantic/library/regex/match_checker/match_checker.h"
#include "compiler/semantic/library/regex/regex_checker/regex_checker.h"
#include "compiler/semantic/library/result/result_checker/result_checker.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/union_checker/union_checker.h"
#include "compiler/semantic/types/unknown_checker/unknown_checker.h"
#include "compiler/semantic/types/utility_types/utility_types.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

constexpr llvm::StringLiteral kThisName = "this";
constexpr llvm::StringLiteral kSuperName = "super";

const GlobalName kConstantHolders[] = {
    GlobalName::CONSOLE,
    GlobalName::MATH,
    GlobalName::STRING,
    GlobalName::NUMBER,
};

}

MemberChecker::MemberChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.scopes;
  this->callChecker = &all.callChecker;
  this->dateChecker = &all.dateChecker;
  this->dictChecker = &all.dictChecker;
  this->exprChecker = &all.exprChecker;
  this->indexProofs = &all.indexProofs;
  this->intlChecker = &all.intlChecker;
  this->jsonChecker = &all.jsonChecker;
  this->bytesChecker = &all.bytesChecker;
  this->matchChecker = &all.matchChecker;
  this->regexChecker = &all.regexChecker;
  this->tupleChecker = &all.tupleChecker;
  this->typeDeclarer = &all.typeDeclarer;
  this->unionChecker = &all.unionChecker;
  this->utilityTypes = &all.utilityTypes;
  this->cryptoChecker = &all.cryptoChecker;
  this->objectChecker = &all.objectChecker;
  this->resultChecker = &all.resultChecker;
  this->staticChecker = &all.staticChecker;
  this->typeRelations = &all.typeRelations;
  this->argListChecker = &all.argListChecker;
  this->constantFolder = &all.constantFolder;
  this->genericChecker = &all.genericChecker;
  this->processChecker = &all.processChecker;
  this->unknownChecker = &all.unknownChecker;
  this->overloadResolver = &all.overloadResolver;
  this->keyedFieldChecker = &all.keyedFieldChecker;
  this->classMemberChecker = &all.classMemberChecker;
  this->builtinModuleChecker = &all.builtinModuleChecker;
  this->runtimeObjectChecker = &all.runtimeObjectChecker;
}

MemberChecker::ResultMemberReadable MemberChecker::checkMember(MemberExpr *member,
                                                               const Type *expected) {
  member->type = Type::error();
  auto *part = this->argListChecker->listPart(member);
  if (part != nullptr) {
    return ok(part);
  }
  std::optional<ResultMemberReadable> scoped = this->scopeObjectMember(member, expected);
  if (scoped.has_value()) {
    return *scoped;
  }
  std::optional<ResultMemberReadable> global = this->globalObjectMember(member);
  if (global.has_value()) {
    return *global;
  }
  auto *object = dyn_cast<NameExpr>(member->object);
  if (object != nullptr && object->name == kThisName) {
    this->state.getClassContext().rememberThisMember(member->name);
  }
  if (object != nullptr && object->name == kSuperName) {
    auto message = ErrorTextMemberRead::superField(member->name);

    return err(MemberCheckerCheckMemberError{
        .code = MemberCheckerCheckMemberErrorCode::SUPER_MEMBER,
        .message = message,
    });
  }
  auto *checked = this->exprChecker->checkExpr(member->object);
  member->object = this->genericChecker->apparent(checked);
  this->state.getClassContext().forgetThisMember();
  if (member->optional) {
    member->object = this->exprChecker->chainLink(member->object);
  }

  return this->checkTypedMember(member, expected);
}

std::optional<MemberChecker::ResultMemberReadable>
MemberChecker::scopeObjectMember(MemberExpr *member, const Type *expected) {
  auto *name = dyn_cast<NameExpr>(member->object);
  if (name == nullptr) {
    return std::nullopt;
  }
  auto entry = this->scopes->lookup(name->name);
  if (entry.enumDecl() != nullptr) {
    auto read = this->typeDeclarer->checkEnumMember(entry.enumDecl(), member, expected);
    if (read.isOk()) {
      return ok(read.value());
    }
    member->type = Type::error();
    return err(MemberCheckerCheckMemberError{
        .code = MemberCheckerCheckMemberErrorCode::ENUM_MEMBER,
        .message = read.error().message,
    });
  }
  if (entry.cls() != nullptr) {
    auto *read = this->staticChecker->checkStaticMember(member, entry.cls(), expected);
    return ok(read);
  }

  return std::nullopt;
}

std::optional<MemberChecker::ResultMemberReadable>
MemberChecker::globalObjectMember(MemberExpr *member) {
  auto *node = this->builtinModuleChecker->checkNodeMember(member);
  if (node != nullptr) {
    return ok(node);
  }
  auto *io = this->processChecker->checkProcessMember(member);
  if (io != nullptr) {
    return ok(io);
  }
  if (this->callChecker->isGlobalName(member->object, GlobalName::DECIMAL)) {
    auto message = ErrorTextMemberRead::decimal(member->name);

    return err(MemberCheckerCheckMemberError{
        .code = MemberCheckerCheckMemberErrorCode::DECIMAL_FUNCTION,
        .message = message,
    });
  }
  if (this->callChecker->isGlobalName(member->object, GlobalName::REG_EXP)) {
    return ok(this->regexChecker->checkRegExpStaticMember(member));
  }
  if (this->callChecker->isGlobalName(member->object, GlobalName::CRYPTO)) {
    return ok(this->cryptoChecker->checkWebCryptoMember(member));
  }
  if (this->callChecker->isGlobalName(member->object, GlobalName::GLOBAL_THIS)) {
    auto message = this->globalThisMessage(member->name);

    return err(MemberCheckerCheckMemberError{
        .code = MemberCheckerCheckMemberErrorCode::GLOBAL_THIS,
        .message = message,
    });
  }
  auto *bytes = this->bytesChecker->checkBytesStaticMember(member);
  if (bytes != nullptr) {
    return ok(bytes);
  }
  if (this->callChecker->isGlobalName(member->object, GlobalName::INTL)) {
    auto message = ErrorTextMemberRead::intl(member->name);

    return err(MemberCheckerCheckMemberError{
        .code = MemberCheckerCheckMemberErrorCode::INTL_VALUE,
        .message = message,
    });
  }

  return this->constantObjectMember(member);
}

std::string MemberChecker::globalThisMessage(llvm::StringRef name) const {
  if (GlobalNameTable::find(name) == GlobalName::CRYPTO) {
    return ErrorTextMemberRead::crypto();
  }

  return ErrorTextMemberRead::globalThis(name);
}

std::optional<MemberChecker::ResultMemberReadable>
MemberChecker::constantObjectMember(MemberExpr *member) {
  for (GlobalName global : kConstantHolders) {
    if (this->callChecker->isGlobalName(member->object, global)) {
      return this->globalConstantMember(member, global);
    }
  }

  return std::nullopt;
}

MemberChecker::ResultMemberReadable MemberChecker::globalConstantMember(MemberExpr *member,
                                                                        GlobalName global) {
  auto *constant = this->constantFolder->globalConstant(global, member->name, member->loc());
  if (constant != nullptr) {
    return ok(constant);
  }
  bool isNumberFunction = NumberStaticTable::find(member->name).has_value();
  if (global == GlobalName::NUMBER && !isNumberFunction) {
    auto message = ErrorTextMemberRead::numberProp(member->name);

    return err(MemberCheckerCheckMemberError{
        .code = MemberCheckerCheckMemberErrorCode::NUMBER_PROPERTY,
        .message = message,
    });
  }
  auto globalName = GlobalNameTable::getName(global);
  auto message = ErrorTextMemberRead::asValue(globalName, member->name);

  return err(MemberCheckerCheckMemberError{
      .code = MemberCheckerCheckMemberErrorCode::GLOBAL_FUNCTION,
      .message = message,
  });
}

MemberChecker::ResultMemberReadable MemberChecker::checkTypedMember(MemberExpr *member,
                                                                    const Type *expected) {
  const Type *type = member->object->type;
  if (type->isError()) {
    return ok(member);
  }
  if (type->nonOptional()->isJson()) {
    return ok(this->jsonChecker->checkJsonMember(member));
  }
  if (type->isUnknown() || type->isAnyObject() || type->isClassRef()) {
    return this->unknownObjectMember(member);
  }
  if (type->isUnion()) {
    return this->commonBaseMember(member, type, expected);
  }

  return this->memberOfType(member, type, expected);
}

MemberChecker::ResultMemberReadable
MemberChecker::commonBaseMember(MemberExpr *member, const Type *type, const Type *expected) {
  const auto *common = this->unionChecker->commonBaseWith(type, member->name);
  if (common == nullptr) {
    return this->unionObjectMember(member);
  }
  member->object = this->typeRelations->convert(member->object, common);

  return this->memberOfType(member, common, expected);
}

MemberChecker::ResultMemberReadable MemberChecker::unknownObjectMember(MemberExpr *member) {
  auto checked = this->unknownChecker->checkUnknownMember(member);
  if (checked.isOk()) {
    return ok(checked.value());
  }
  bool isNarrow = checked.error().code == UnknownCheckerCheckUnknownMemberErrorCode::NOT_NARROWED;
  auto code = isNarrow ? MemberCheckerCheckMemberErrorCode::UNKNOWN_NOT_NARROWED
                       : MemberCheckerCheckMemberErrorCode::UNKNOWN_MEMBER;

  return err(MemberCheckerCheckMemberError{
      .code = code,
      .message = checked.error().message,
  });
}

MemberChecker::ResultMemberReadable MemberChecker::unionObjectMember(MemberExpr *member) {
  auto fieldRead = this->unionChecker->checkUnionMember(member);
  if (fieldRead.isOk()) {
    return ok(fieldRead.value());
  }
  member->type = Type::error();

  return err(MemberCheckerCheckMemberError{
      .code = MemberCheckerCheckMemberErrorCode::UNION_MEMBER,
      .message = fieldRead.error().message,
  });
}

MemberChecker::ResultMemberReadable
MemberChecker::memberOfType(MemberExpr *member, const Type *type, const Type *expected) {
  if (type->isResult()) {
    return ok(this->resultChecker->checkResultMember(member));
  }
  if (type->isTuple()) {
    return this->tupleObjectMember(member);
  }
  if (type->isClass()) {
    return ok(this->classMemberChecker->checkClassMember(member, expected));
  }
  if (type->isRegExp()) {
    return ok(this->regexChecker->checkRegexMember(member));
  }
  if (type->isMatch()) {
    return ok(this->matchChecker->checkMatchMember(member));
  }
  if (type->isDate()) {
    return ok(this->dateChecker->checkDateMember(member));
  }
  if (type->isBytes()) {
    return ok(this->bytesChecker->checkBytesMember(member));
  }
  if (type->isCrypto()) {
    return ok(this->cryptoChecker->checkCryptoMember(member));
  }
  if (type->isIntl()) {
    return ok(this->intlChecker->checkIntlMember(member, expected));
  }
  if (type->isWeakRef()) {
    auto message = ErrorTextMemberRead::weakRef(member->name);

    return err(MemberCheckerCheckMemberError{
        .code = MemberCheckerCheckMemberErrorCode::WEAK_REF_MEMBER,
        .message = message,
    });
  }
  if (type->isDict()) {
    return this->dictObjectMember(member);
  }
  if (type->isRuntimeObject()) {
    return ok(this->runtimeObjectChecker->checkRuntimeMember(member));
  }
  if (type->isObject() && type->overload(member->name) != nullptr) {
    return ok(this->overloadResolver->checkOverloadMember(member, expected));
  }
  if (type->isObject()) {
    return this->objectFieldMember(member);
  }

  return this->builtinValueMember(member, type);
}

MemberChecker::ResultMemberReadable MemberChecker::tupleObjectMember(MemberExpr *member) {
  auto tupleMember = this->tupleChecker->checkTupleMember(member);
  if (tupleMember.isErr()) {
    return err(MemberCheckerCheckMemberError{
        .code = MemberCheckerCheckMemberErrorCode::TUPLE_MEMBER,
        .message = tupleMember.error().message,
    });
  }

  return ok(tupleMember.value());
}

MemberChecker::ResultMemberReadable MemberChecker::dictObjectMember(MemberExpr *member) {
  auto dict = this->dictChecker->checkDictMember(member);
  if (dict.isErr()) {
    return err(MemberCheckerCheckMemberError{
        .code = MemberCheckerCheckMemberErrorCode::DICT_MEMBER,
        .message = dict.error().message,
    });
  }

  return ok(dict.value());
}

MemberChecker::ResultMemberReadable MemberChecker::objectFieldMember(MemberExpr *member) {
  auto field = this->objectChecker->checkField(member);
  if (field.isErr()) {
    return err(MemberCheckerCheckMemberError{
        .code = MemberCheckerCheckMemberErrorCode::OBJECT_FIELD,
        .message = field.error().message,
    });
  }

  return ok(field.value());
}

MemberChecker::ResultMemberReadable MemberChecker::builtinValueMember(MemberExpr *member,
                                                                      const Type *type) {
  bool isSized = type->isString() || type->isArray() || type->isListParam();
  if (isSized && BuiltinPropertyTable::isName(member->name, BuiltinProperty::LENGTH)) {
    member->type = Type::intTy();
    return ok(member);
  }
  bool isWrappedValue = BuiltinPropertyTable::isName(member->name, BuiltinProperty::WRAPPED_VALUE);
  if (type->isInteger() && type->isWrapping() && isWrappedValue) {
    const auto *plain = Type::unsignedInteger(type->bitWidth());
    auto *value = this->typeRelations->convert(member->object, plain);

    return ok(value);
  }
  if (type->isOptional()) {
    auto message = ErrorTextMemberRead::undefined(member->name);

    return err(MemberCheckerCheckMemberError{
        .code = MemberCheckerCheckMemberErrorCode::MAY_BE_UNDEFINED,
        .message = message,
    });
  }
  bool isRaw = BuiltinPropertyTable::isName(member->name, BuiltinProperty::RAW_TEXTS);
  if (type == Type::array(Type::string()) && isRaw) {
    auto message = ErrorTextMemberRead::raw();

    return err(MemberCheckerCheckMemberError{
        .code = MemberCheckerCheckMemberErrorCode::RAW_TEXTS,
        .message = message,
    });
  }
  if (type->isArray() || type->isString() || type->isIterator()) {
    auto message = ErrorTextMemberRead::mustCall(member->name, type->name());

    return err(MemberCheckerCheckMemberError{
        .code = MemberCheckerCheckMemberErrorCode::METHOD_NOT_CALLED,
        .message = message,
    });
  }
  auto message = ErrorTextMemberRead::noProperty(member->name, type->name());

  return err(MemberCheckerCheckMemberError{
      .code = MemberCheckerCheckMemberErrorCode::NO_SUCH_PROPERTY,
      .message = message,
  });
}

MemberChecker::ResultIndexIsInteger MemberChecker::checkPositionType(const Type *type) const {
  if (!type->isInteger()) {
    auto message = ErrorTextPosition::notInt(type->name(), type->isFloat(), type->isOptional());

    return err(MemberCheckerPositionTypeError{
        .code = MemberCheckerPositionTypeErrorCode::NOT_AN_INTEGER,
        .message = message,
    });
  }

  return ok(type);
}

Expr *MemberChecker::checkPosition(Expr *expr) {
  auto *checked = this->exprChecker->checkExpr(expr, Type::intTy());
  auto *position = this->genericChecker->apparent(checked);
  const Type *type = position->type;
  if (type->isError()) {
    return position;
  }
  auto integer = this->checkPositionType(type);
  if (integer.isErr()) {
    this->state.getDiagnostics().error(position->loc(), integer.error().message);
    return position;
  }

  return this->typeRelations->convert(position, Type::intTy());
}

MemberChecker::ResultIndexedElement MemberChecker::checkIndex(IndexExpr *index) {
  auto *part = this->argListChecker->listPart(index);
  if (part != nullptr) {
    return ok(part);
  }
  auto *enumDecl = this->indexedEnum(index);
  if (enumDecl != nullptr) {
    return this->enumIndex(index, enumDecl);
  }
  index->object = this->builtinModuleChecker->processAlias(index->object);
  auto *variable = this->processChecker->checkProcessEnvIndex(index);
  if (variable != nullptr) {
    return ok(variable);
  }
  auto *checked = this->exprChecker->checkExpr(index->object);
  index->object = this->genericChecker->apparent(checked);
  if (index->optional) {
    index->object = this->exprChecker->chainLink(index->object);
  }
  index->type = Type::error();

  return this->checkTypedIndex(index);
}

EnumDecl *MemberChecker::indexedEnum(const IndexExpr *index) {
  auto *name = dyn_cast<NameExpr>(index->object);
  if (name == nullptr) {
    return nullptr;
  }

  return this->scopes->lookup(name->name).enumDecl();
}

MemberChecker::ResultIndexedElement MemberChecker::enumIndex(IndexExpr *index, EnumDecl *enumDecl) {
  auto read = this->typeDeclarer->checkEnumIndex(enumDecl, index);
  if (read.isOk()) {
    return ok(read.value());
  }
  auto code = this->enumIndexCode(read.error().code);

  return err(MemberCheckerCheckIndexError{
      .code = code,
      .message = read.error().message,
  });
}

MemberCheckerCheckIndexErrorCode
MemberChecker::enumIndexCode(TypeDeclarerCheckEnumIndexErrorCode code) const {
  switch (code) {
    case TypeDeclarerCheckEnumIndexErrorCode::ALREADY_REPORTED:
      return MemberCheckerCheckIndexErrorCode::ALREADY_REPORTED;
    case TypeDeclarerCheckEnumIndexErrorCode::STRING_ENUM:
      return MemberCheckerCheckIndexErrorCode::ENUM_STRING;
    case TypeDeclarerCheckEnumIndexErrorCode::NOT_AN_INTEGER:
      return MemberCheckerCheckIndexErrorCode::ENUM_NOT_INTEGER;
  }

  return MemberCheckerCheckIndexErrorCode::ALREADY_REPORTED;
}

MemberChecker::ResultIndexedElement MemberChecker::checkTypedIndex(IndexExpr *index) {
  auto generic = this->utilityTypes->checkGenericIndex(index);
  bool isGenericIndex =
      generic.isOk() ||
      generic.error().code != UtilityTypesCheckGenericIndexErrorCode::NOT_A_GENERIC_INDEX;
  if (generic.isErr() && isGenericIndex) {
    return err(MemberCheckerCheckIndexError{
        .code = MemberCheckerCheckIndexErrorCode::GENERIC_INDEX,
        .message = generic.error().message,
    });
  }
  if (isGenericIndex) {
    return ok(generic.value());
  }
  const Type *type = index->object->type;
  if (type->isMatch()) {
    return ok(this->matchChecker->checkMatchIndex(index));
  }
  if (type->nonOptional()->isJson()) {
    return ok(this->jsonChecker->checkJsonIndex(index));
  }
  if (type->isBytes()) {
    return ok(this->bytesChecker->checkBytesIndex(index));
  }
  if (type->isUnknown() || type->isAnyObject()) {
    return this->unknownIndex(index);
  }
  if (type->isObject()) {
    return this->keyedIndex(index);
  }
  if (type->isDict()) {
    return this->dictIndex(index);
  }
  if (type->isOptional() && type->inner()->isObject()) {
    index->index = this->exprChecker->checkExpr(index->index);
    auto message = ErrorTextIndexRead::undefined();

    return err(MemberCheckerCheckIndexError{
        .code = MemberCheckerCheckIndexErrorCode::VALUE_MAY_BE_UNDEFINED,
        .message = message,
    });
  }

  return this->positionIndex(index);
}

MemberChecker::ResultIndexedElement MemberChecker::unknownIndex(IndexExpr *index) {
  index->index = this->exprChecker->checkExpr(index->index);
  auto narrowed = this->unknownChecker->unknownUse(index->object, "");
  if (narrowed.isErr()) {
    return err(MemberCheckerCheckIndexError{
        .code = MemberCheckerCheckIndexErrorCode::UNKNOWN_NOT_NARROWED,
        .message = narrowed.error().message,
    });
  }

  return ok(index);
}

MemberChecker::ResultIndexedElement MemberChecker::keyedIndex(IndexExpr *index) {
  auto keyed = this->keyedFieldChecker->checkKeyedField(index, KeyedAccess::READ);
  if (keyed.isOk()) {
    return ok(keyed.value());
  }
  auto code = this->keyedIndexCode(keyed.error().code);

  return err(MemberCheckerCheckIndexError{
      .code = code,
      .message = keyed.error().message,
  });
}

MemberCheckerCheckIndexErrorCode
MemberChecker::keyedIndexCode(KeyedFieldCheckerCheckKeyedFieldErrorCode code) const {
  if (code == KeyedFieldCheckerCheckKeyedFieldErrorCode::ALREADY_REPORTED) {
    return MemberCheckerCheckIndexErrorCode::ALREADY_REPORTED;
  }
  if (this->keyedFieldChecker->isWrongKey(code)) {
    return MemberCheckerCheckIndexErrorCode::KEY_REJECTED;
  }

  return MemberCheckerCheckIndexErrorCode::FIELD_REJECTED;
}

MemberChecker::ResultIndexedElement MemberChecker::dictIndex(IndexExpr *index) {
  auto dict = this->dictChecker->checkDictIndex(index);
  if (dict.isErr()) {
    return err(MemberCheckerCheckIndexError{
        .code = MemberCheckerCheckIndexErrorCode::DICT_INDEX,
        .message = dict.error().message,
    });
  }

  return ok(dict.value());
}

MemberChecker::ResultIndexedElement MemberChecker::positionIndex(IndexExpr *index) {
  index->index = this->checkPosition(index->index);
  const Type *type = index->object->type;
  if (type->isError() || index->index->type->isError()) {
    return ok(index);
  }
  if (type->isListParam()) {
    return this->listParamIndex(index, type);
  }
  const auto *element = this->indexedElement(type);
  bool isOptionalList =
      type->isOptional() && (type->inner()->isArray() || type->inner()->isString());
  if (element == nullptr && isOptionalList) {
    auto message = ErrorTextIndexRead::undefined();

    return err(MemberCheckerCheckIndexError{
        .code = MemberCheckerCheckIndexErrorCode::VALUE_MAY_BE_UNDEFINED,
        .message = message,
    });
  }
  if (element == nullptr) {
    auto message = ErrorTextIndexRead::notIndexed(type->name());

    return err(MemberCheckerCheckIndexError{
        .code = MemberCheckerCheckIndexErrorCode::NOT_INDEXABLE,
        .message = message,
    });
  }
  bool isInRange = this->indexProofs->canProveInBounds(index->object, index->index);
  ElementIndex *access = index->form.as<ElementIndex>();
  if (access != nullptr) {
    access->mayBeOutOfRange = !isInRange;
  }
  index->type = isInRange ? element : Type::optional(element);

  return ok(index);
}

const Type *MemberChecker::indexedElement(const Type *type) const {
  if (type->isString()) {
    return type;
  }
  if (type->isArray()) {
    return type->inner();
  }

  return nullptr;
}

MemberChecker::ResultIndexedElement MemberChecker::listParamIndex(IndexExpr *index,
                                                                  const Type *type) {
  const auto *at = ExprParts::withoutImplicitCasts(index->index);
  auto *literal = dyn_cast<IntLitExpr>(at);
  if (literal == nullptr) {
    const auto *element = this->argListChecker->listElement(type);
    index->type = Type::optional(element);
    return ok(index);
  }
  const auto *position = Type::numberLiteral(double(literal->value), Type::intTy());
  index->type = Type::indexedAccess(type, position);

  return ok(index);
}

}
