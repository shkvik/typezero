#include "compiler/semantic/bodies/expr/member_checker/member_checker.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
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
  Expr *part = this->argListChecker->listPart(member);
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
  if (object != nullptr && object->name == "this") {
    this->state.getClassContext().rememberThisMember(member->name);
  }
  if (object != nullptr && object->name == "super") {
    std::string message = ErrorTextMemberRead::superField(member->name);

    return err(MemberCheckerCheckMemberError{
        .code = MemberCheckerCheckMemberErrorCode::SUPER_MEMBER,
        .message = message,
    });
  }
  Expr *checked = this->exprChecker->checkExpr(member->object);
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
  ScopeEntry entry = this->scopes->lookup(name->name);
  if (entry.enumDecl() != nullptr) {
    TypeDeclarer::ResultEnumMemberRead read =
        this->typeDeclarer->checkEnumMember(entry.enumDecl(), member, expected);
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
    Expr *read = this->staticChecker->checkStaticMember(member, entry.cls(), expected);
    return ok(read);
  }

  return std::nullopt;
}

std::optional<MemberChecker::ResultMemberReadable>
MemberChecker::globalObjectMember(MemberExpr *member) {
  Expr *node = this->builtinModuleChecker->checkNodeMember(member);
  if (node != nullptr) {
    return ok(node);
  }
  Expr *io = this->processChecker->checkProcessMember(member);
  if (io != nullptr) {
    return ok(io);
  }
  if (this->callChecker->isGlobalName(member->object, GlobalName::DECIMAL)) {
    std::string message = ErrorTextMemberRead::decimal(member->name);

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
    std::string message = member->name == "crypto" ? ErrorTextMemberRead::crypto()
                                                   : ErrorTextMemberRead::globalThis(member->name);

    return err(MemberCheckerCheckMemberError{
        .code = MemberCheckerCheckMemberErrorCode::GLOBAL_THIS,
        .message = message,
    });
  }
  Expr *bytes = this->bytesChecker->checkBytesStaticMember(member);
  if (bytes != nullptr) {
    return ok(bytes);
  }
  if (this->callChecker->isGlobalName(member->object, GlobalName::INTL)) {
    std::string message = ErrorTextMemberRead::intl(member->name);

    return err(MemberCheckerCheckMemberError{
        .code = MemberCheckerCheckMemberErrorCode::INTL_VALUE,
        .message = message,
    });
  }

  return this->constantObjectMember(member);
}

std::optional<MemberChecker::ResultMemberReadable>
MemberChecker::constantObjectMember(MemberExpr *member) {
  static const GlobalName withConstants[] = {GlobalName::CONSOLE, GlobalName::MATH,
                                             GlobalName::STRING, GlobalName::NUMBER};
  for (GlobalName global : withConstants) {
    if (this->callChecker->isGlobalName(member->object, global)) {
      return this->globalConstantMember(member, global);
    }
  }

  return std::nullopt;
}

MemberChecker::ResultMemberReadable MemberChecker::globalConstantMember(MemberExpr *member,
                                                                        GlobalName global) {
  Expr *constant = this->constantFolder->globalConstant(global, member->name, member->loc());
  if (constant != nullptr) {
    return ok(constant);
  }
  static const llvm::StringLiteral numberFunctions[] = {"parseInt", "parseFloat", "isInteger",
                                                        "isSafeInteger"};
  if (global == GlobalName::NUMBER && !llvm::is_contained(numberFunctions, member->name)) {
    std::string message = ErrorTextMemberRead::numberProp(member->name);

    return err(MemberCheckerCheckMemberError{
        .code = MemberCheckerCheckMemberErrorCode::NUMBER_PROPERTY,
        .message = message,
    });
  }
  llvm::StringRef globalName = GlobalNameTable::getName(global);
  std::string message = ErrorTextMemberRead::asValue(globalName, member->name);

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
    const Type *common = this->unionChecker->commonBaseWith(type, member->name);
    if (common == nullptr) {
      return this->unionObjectMember(member);
    }
    member->object = this->typeRelations->convert(member->object, common);
    type = common;
  }

  return this->memberOfType(member, type, expected);
}

MemberChecker::ResultMemberReadable MemberChecker::unknownObjectMember(MemberExpr *member) {
  UnknownChecker::ResultMemberChecked checked = this->unknownChecker->checkUnknownMember(member);
  if (checked.isOk()) {
    return ok(checked.value());
  }
  bool isNarrow = checked.error().code == UnknownCheckerCheckUnknownMemberErrorCode::NOT_NARROWED;

  return err(MemberCheckerCheckMemberError{
      .code = isNarrow ? MemberCheckerCheckMemberErrorCode::UNKNOWN_NOT_NARROWED
                       : MemberCheckerCheckMemberErrorCode::UNKNOWN_MEMBER,
      .message = checked.error().message,
  });
}

MemberChecker::ResultMemberReadable MemberChecker::unionObjectMember(MemberExpr *member) {
  UnionChecker::ResultUnionMemberTyped fieldRead = this->unionChecker->checkUnionMember(member);
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
    std::string message = ErrorTextMemberRead::weakRef(member->name);

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
  TupleChecker::ResultTupleMember tupleMember = this->tupleChecker->checkTupleMember(member);
  if (tupleMember.isErr()) {
    return err(MemberCheckerCheckMemberError{
        .code = MemberCheckerCheckMemberErrorCode::TUPLE_MEMBER,
        .message = tupleMember.error().message,
    });
  }

  return ok(tupleMember.value());
}

MemberChecker::ResultMemberReadable MemberChecker::dictObjectMember(MemberExpr *member) {
  DictChecker::ResultDictMemberAccess dict = this->dictChecker->checkDictMember(member);
  if (dict.isErr()) {
    return err(MemberCheckerCheckMemberError{
        .code = MemberCheckerCheckMemberErrorCode::DICT_MEMBER,
        .message = dict.error().message,
    });
  }

  return ok(dict.value());
}

MemberChecker::ResultMemberReadable MemberChecker::objectFieldMember(MemberExpr *member) {
  ObjectChecker::ResultObjectFieldValue field = this->objectChecker->checkField(member);
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
  if (type->isInteger() && type->isWrapping() && member->name == "value") {
    const Type *plain = Type::integer(type->bitWidth(), true);
    return ok(this->typeRelations->convert(member->object, plain));
  }
  if (type->isOptional()) {
    std::string message = ErrorTextMemberRead::undefined(member->name);

    return err(MemberCheckerCheckMemberError{
        .code = MemberCheckerCheckMemberErrorCode::MAY_BE_UNDEFINED,
        .message = message,
    });
  }
  if (type == Type::array(Type::string()) && member->name == "raw") {
    std::string message = ErrorTextMemberRead::raw();

    return err(MemberCheckerCheckMemberError{
        .code = MemberCheckerCheckMemberErrorCode::RAW_TEXTS,
        .message = message,
    });
  }
  if (type->isArray() || type->isString() || type->isIterator()) {
    std::string message = ErrorTextMemberRead::mustCall(member->name, type->name());

    return err(MemberCheckerCheckMemberError{
        .code = MemberCheckerCheckMemberErrorCode::METHOD_NOT_CALLED,
        .message = message,
    });
  }
  std::string message = ErrorTextMemberRead::noProperty(member->name, type->name());

  return err(MemberCheckerCheckMemberError{
      .code = MemberCheckerCheckMemberErrorCode::NO_SUCH_PROPERTY,
      .message = message,
  });
}

MemberChecker::ResultIndexIsInteger MemberChecker::positionType(const Type *type) {
  if (!type->isInteger()) {
    std::string message =
        ErrorTextPosition::notInt(type->name(), type->isFloat(), type->isOptional());

    return err(MemberCheckerPositionTypeError{
        .code = MemberCheckerPositionTypeErrorCode::NOT_AN_INTEGER,
        .message = message,
    });
  }

  return ok(type);
}

Expr *MemberChecker::checkPosition(Expr *expr) {
  expr = this->genericChecker->apparent(this->exprChecker->checkExpr(expr, Type::intTy()));
  const Type *type = expr->type;
  if (type->isError()) {
    return expr;
  }
  ResultIndexIsInteger integer = MemberChecker::positionType(type);
  if (integer.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), integer.error().message);
    return expr;
  }

  return this->typeRelations->convert(expr, Type::intTy());
}

MemberChecker::ResultIndexedElement MemberChecker::checkIndex(IndexExpr *index) {
  Expr *part = this->argListChecker->listPart(index);
  if (part != nullptr) {
    return ok(part);
  }
  EnumDecl *decl = this->indexedEnum(index);
  if (decl != nullptr) {
    return this->enumIndex(index, decl);
  }
  index->object = this->builtinModuleChecker->processAlias(index->object);
  Expr *variable = this->processChecker->checkProcessEnvIndex(index);
  if (variable != nullptr) {
    return ok(variable);
  }
  Expr *checked = this->exprChecker->checkExpr(index->object);
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

MemberChecker::ResultIndexedElement MemberChecker::enumIndex(IndexExpr *index, EnumDecl *decl) {
  TypeDeclarer::ResultEnumIndexValue read = this->typeDeclarer->checkEnumIndex(decl, index);
  if (read.isOk()) {
    return ok(read.value());
  }
  MemberCheckerCheckIndexErrorCode code = MemberCheckerCheckIndexErrorCode::ALREADY_REPORTED;
  switch (read.error().code) {
    case TypeDeclarerCheckEnumIndexErrorCode::ALREADY_REPORTED:
      code = MemberCheckerCheckIndexErrorCode::ALREADY_REPORTED;
      break;
    case TypeDeclarerCheckEnumIndexErrorCode::STRING_ENUM:
      code = MemberCheckerCheckIndexErrorCode::ENUM_STRING;
      break;
    case TypeDeclarerCheckEnumIndexErrorCode::NOT_AN_INTEGER:
      code = MemberCheckerCheckIndexErrorCode::ENUM_NOT_INTEGER;
      break;
  }

  return err(MemberCheckerCheckIndexError{
      .code = code,
      .message = read.error().message,
  });
}

MemberChecker::ResultIndexedElement MemberChecker::checkTypedIndex(IndexExpr *index) {
  UtilityTypes::ResultGenericIndexed generic = this->utilityTypes->checkGenericIndex(index);
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
    std::string message = ErrorTextIndexRead::undefined();

    return err(MemberCheckerCheckIndexError{
        .code = MemberCheckerCheckIndexErrorCode::VALUE_MAY_BE_UNDEFINED,
        .message = message,
    });
  }

  return this->positionIndex(index);
}

MemberChecker::ResultIndexedElement MemberChecker::unknownIndex(IndexExpr *index) {
  index->index = this->exprChecker->checkExpr(index->index);
  UnknownChecker::ResultValueNarrowed narrowed = UnknownChecker::unknownUse(index->object, "");
  if (narrowed.isErr()) {
    return err(MemberCheckerCheckIndexError{
        .code = MemberCheckerCheckIndexErrorCode::UNKNOWN_NOT_NARROWED,
        .message = narrowed.error().message,
    });
  }

  return ok(index);
}

MemberChecker::ResultIndexedElement MemberChecker::keyedIndex(IndexExpr *index) {
  KeyedFieldChecker::ResultKeyedFieldRead keyed =
      this->keyedFieldChecker->checkKeyedField(index, false);
  if (keyed.isOk()) {
    return ok(keyed.value());
  }
  KeyedFieldCheckerCheckKeyedFieldErrorCode keyedCode = keyed.error().code;
  bool isReported = keyedCode == KeyedFieldCheckerCheckKeyedFieldErrorCode::ALREADY_REPORTED;
  MemberCheckerCheckIndexErrorCode code = MemberCheckerCheckIndexErrorCode::FIELD_REJECTED;
  if (isReported) {
    code = MemberCheckerCheckIndexErrorCode::ALREADY_REPORTED;
  } else if (KeyedFieldChecker::wrongKey(keyedCode)) {
    code = MemberCheckerCheckIndexErrorCode::KEY_REJECTED;
  }

  return err(MemberCheckerCheckIndexError{
      .code = code,
      .message = keyed.error().message,
  });
}

MemberChecker::ResultIndexedElement MemberChecker::dictIndex(IndexExpr *index) {
  DictChecker::ResultDictIndexedValue dict = this->dictChecker->checkDictIndex(index);
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
  const Type *element = type->isString() ? type : type->isArray() ? type->inner() : nullptr;
  bool isOptionalList =
      type->isOptional() && (type->inner()->isArray() || type->inner()->isString());
  if (element == nullptr && isOptionalList) {
    std::string message = ErrorTextIndexRead::undefined();

    return err(MemberCheckerCheckIndexError{
        .code = MemberCheckerCheckIndexErrorCode::VALUE_MAY_BE_UNDEFINED,
        .message = message,
    });
  }
  if (element == nullptr) {
    std::string message = ErrorTextIndexRead::notIndexed(type->name());

    return err(MemberCheckerCheckIndexError{
        .code = MemberCheckerCheckIndexErrorCode::NOT_INDEXABLE,
        .message = message,
    });
  }
  bool isInRange = this->indexProofs->provesInBounds(index->object, index->index, false);
  ElementIndex *access = index->form.as<ElementIndex>();
  if (access != nullptr) {
    access->mayBeOutOfRange = !isInRange;
  }
  index->type = isInRange ? element : Type::optional(element);

  return ok(index);
}

MemberChecker::ResultIndexedElement MemberChecker::listParamIndex(IndexExpr *index,
                                                                  const Type *type) {
  const Expr *at = index->index;
  auto *cast = dyn_cast<CastExpr>(at);
  while (cast != nullptr && cast->isImplicit()) {
    at = cast->operand;
    cast = dyn_cast<CastExpr>(at);
  }
  auto *literal = dyn_cast<IntLitExpr>(at);
  if (literal != nullptr) {
    const Type *position = Type::numberLiteral(double(literal->value), Type::intTy());
    index->type = Type::indexedAccess(type, position);
  } else {
    index->type = Type::optional(this->argListChecker->listElement(type));
  }

  return ok(index);
}

}
