#include "compiler/semantic/code/members/code_members_registry.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/program/library/builtin_names/global_constant_table/global_constant_table.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/builtin_names/keyword_table/keyword_table.h"
#include "compiler/program/library/builtin_names/type_name_table/type_name_table.h"
#include "compiler/program/library/number/number_static_table/number_static_table.h"
#include "compiler/program/to_utf16/to_utf16.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/types/type_parts/type_routes.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include <cmath>
#include <cstdio>
#include <string>

using llvm::dyn_cast;

namespace tz {

namespace {

const GlobalName kConstantHolders[] = {
    GlobalName::CONSOLE,
    GlobalName::MATH,
    GlobalName::STRING,
    GlobalName::NUMBER,
};

}

CodeMembersRegistry::CodeMembersRegistry(SemanticState &owner)
    : state(owner) {}

void CodeMembersRegistry::checkMember(const CheckMember &task) {
  task.member->type = Type::error();
  this->state.walk.tasks.pushTask(AfterListPart{.member = task.member, .expected = task.expected});

  return this->state.walk.tasks.pushTask(FindListPart{.expr = task.member});
}

void CodeMembersRegistry::afterListPart(const AfterListPart &task) {
  MemberExpr *member = task.member;
  Expr *part = this->state.walk.tasks.takeChecked();
  if (part != nullptr) {
    return this->state.walk.tasks.pushChecked(part);
  }
  auto *name = dyn_cast<NameExpr>(member->object);
  auto entry = name != nullptr ? this->state.walk.scopes.findName(name->name) : ScopeEntry();
  if (entry.enumDecl() != nullptr) {
    this->state.walk.tasks.pushTask(AfterEnumResolved{
        .member = member,
        .enumDecl = entry.enumDecl(),
        .expected = task.expected,
    });
    return this->state.walk.tasks.pushTask(ResolveEnum{.enumDecl = entry.enumDecl()});
  }
  if (entry.cls() != nullptr) {
    return this->state.walk.tasks.pushTask(CheckStaticMember{
        .member = member,
        .classDecl = entry.cls(),
        .expected = task.expected,
    });
  }
  this->state.walk.tasks.pushTask(AfterNodeMember{.member = member, .expected = task.expected});

  return this->state.walk.tasks.pushTask(CheckNodeMember{.member = member});
}

void CodeMembersRegistry::afterNodeMember(const AfterNodeMember &task) {
  Expr *node = this->state.walk.tasks.takeChecked();
  if (node != nullptr) {
    return this->state.walk.tasks.pushChecked(node);
  }
  this->state.walk.tasks.pushTask(
      AfterProcessMember{.member = task.member, .expected = task.expected});

  return this->state.walk.tasks.pushTask(CheckProcessMember{.member = task.member});
}

void CodeMembersRegistry::afterProcessMember(const AfterProcessMember &task) {
  MemberExpr *member = task.member;
  Expr *io = this->state.walk.tasks.takeChecked();
  if (io != nullptr) {
    return this->state.walk.tasks.pushChecked(io);
  }
  if (this->state.isGlobalName(member->object, GlobalName::DECIMAL)) {
    auto message = ErrorTextMemberRead::decimal(member->name);
    return this->reportMember(member, CodeMembersRegistryCheckMemberErrorCode::DECIMAL_FUNCTION,
                              message);
  }
  if (this->state.isGlobalName(member->object, GlobalName::REG_EXP)) {
    return this->state.walk.tasks.pushTask(CheckRegExpStaticMember{.member = member});
  }
  if (this->state.isGlobalName(member->object, GlobalName::CRYPTO)) {
    return this->state.walk.tasks.pushTask(CheckWebCryptoMember{.member = member});
  }
  if (this->state.isGlobalName(member->object, GlobalName::GLOBAL_THIS)) {
    auto message = this->globalThisMessage(member->name);
    return this->reportMember(member, CodeMembersRegistryCheckMemberErrorCode::GLOBAL_THIS,
                              message);
  }
  this->state.walk.tasks.pushTask(
      AfterBytesStaticMember{.member = member, .expected = task.expected});

  return this->state.walk.tasks.pushTask(CheckBytesStaticMember{.member = member});
}

void CodeMembersRegistry::afterBytesStaticMember(const AfterBytesStaticMember &task) {
  MemberExpr *member = task.member;
  Expr *bytes = this->state.walk.tasks.takeChecked();
  if (bytes != nullptr) {
    return this->state.walk.tasks.pushChecked(bytes);
  }
  if (this->state.isGlobalName(member->object, GlobalName::INTL)) {
    auto message = ErrorTextMemberRead::intl(member->name);
    return this->reportMember(member, CodeMembersRegistryCheckMemberErrorCode::INTL_VALUE, message);
  }
  auto constant = this->constantObjectMember(member);
  if (constant.has_value()) {
    return this->pushMemberRead(member, *constant);
  }
  auto *object = dyn_cast<NameExpr>(member->object);
  if (object != nullptr && object->name == KeywordTable::getName(Keyword::THIS)) {
    this->state.walk.self.rememberThisMember(member->name);
  }
  if (object != nullptr && object->name == KeywordTable::getName(Keyword::SUPER)) {
    auto message = ErrorTextMemberRead::superField(member->name);
    return this->reportMember(member, CodeMembersRegistryCheckMemberErrorCode::SUPER_MEMBER,
                              message);
  }
  this->state.walk.tasks.pushTask(AfterMemberObject{.member = member, .expected = task.expected});
  this->state.walk.tasks.pushTask(ApparentSlot{.slot = &member->object});

  return this->state.walk.tasks.pushTask(
      CheckExprSlot{.slot = &member->object, .expected = nullptr});
}

void CodeMembersRegistry::pushMemberRead(MemberExpr *member, const ResultMemberReadable &read) {
  if (read.isOk()) {
    return this->state.walk.tasks.pushChecked(read.value());
  }

  return this->reportMember(member, read.error().code, read.error().message);
}

void CodeMembersRegistry::reportMember(MemberExpr *member,
                                       CodeMembersRegistryCheckMemberErrorCode code,
                                       const std::string &message) {
  bool isOnObject = code == CodeMembersRegistryCheckMemberErrorCode::MAY_BE_UNDEFINED ||
                    code == CodeMembersRegistryCheckMemberErrorCode::UNKNOWN_NOT_NARROWED;
  llvm::SMLoc at = isOnObject ? member->object->loc() : member->loc();
  this->state.diagnostics.error(at, message);

  return this->state.walk.tasks.pushChecked(member);
}

void CodeMembersRegistry::afterMemberObject(const AfterMemberObject &task) {
  MemberExpr *member = task.member;
  this->state.walk.self.forgetThisMember();
  if (member->optional) {
    member->object = this->chainLink(member->object);
  }
  const Type *type = member->object->type;
  if (type->isError()) {
    return this->state.walk.tasks.pushChecked(member);
  }
  if (type->nonOptional()->isJson()) {
    return this->state.walk.tasks.pushTask(CheckJsonMember{.member = member});
  }
  if (type->isUnknown() || type->isAnyObject() || type->isClassRef()) {
    return this->state.walk.tasks.pushTask(CheckUnknownMember{.member = member});
  }
  if (!type->isUnion()) {
    return this->memberOfType(member, type, task.expected);
  }
  const auto *common = this->state.types.relations.commonBaseWith(type, member->name);
  if (common == nullptr) {
    return this->state.walk.tasks.pushTask(CheckUnionMember{.member = member});
  }
  this->state.walk.tasks.pushTask(MemberOfCommonBase{
      .member = member,
      .common = common,
      .expected = task.expected,
  });

  return this->state.walk.tasks.pushTask(ConvertSlot{.slot = &member->object, .to = common});
}

void CodeMembersRegistry::memberOfCommonBase(const MemberOfCommonBase &task) {
  return this->memberOfType(task.member, task.common, task.expected);
}

void CodeMembersRegistry::memberOfType(MemberExpr *member, const Type *type, const Type *expected) {
  auto &tasks = this->state.walk.tasks;
  if (type->isResult()) {
    return tasks.pushTask(CheckResultMember{.member = member});
  }
  if (type->isTuple()) {
    return tasks.pushTask(CheckTupleMember{.member = member});
  }
  if (type->isClass()) {
    return tasks.pushTask(CheckClassMember{.member = member, .expected = expected});
  }
  if (type->isRegExp()) {
    return tasks.pushTask(CheckRegexMember{.member = member});
  }
  if (type->isMatch()) {
    return tasks.pushTask(CheckMatchMember{.member = member});
  }
  if (type->isDate()) {
    return tasks.pushTask(CheckDateMember{.member = member});
  }
  if (type->isBytes()) {
    return tasks.pushTask(CheckBytesMember{.member = member});
  }
  if (type->isCrypto()) {
    return tasks.pushTask(CheckCryptoMember{.member = member});
  }
  if (type->isIntl()) {
    return tasks.pushTask(CheckIntlMember{.member = member, .expected = expected});
  }
  if (type->isWeakRef()) {
    auto message = ErrorTextMemberRead::weakRef(member->name);
    return this->reportMember(member, CodeMembersRegistryCheckMemberErrorCode::WEAK_REF_MEMBER,
                              message);
  }
  if (type->isDict()) {
    return tasks.pushTask(CheckDictMember{.member = member});
  }
  if (type->isRuntimeObject()) {
    return tasks.pushTask(CheckRuntimeMember{.member = member});
  }
  if (type->isObject() && type->overload(member->name) != nullptr) {
    return tasks.pushTask(CheckOverloadMember{.member = member, .expected = expected});
  }
  if (type->isObject()) {
    return tasks.pushTask(CheckField{.member = member});
  }

  return this->walkBuiltinValueMember(member, type);
}

void CodeMembersRegistry::walkBuiltinValueMember(MemberExpr *member, const Type *type) {
  bool isSized = type->isString() || type->isArray() || type->isListParam();
  if (isSized && BuiltinPropertyTable::isName(member->name, BuiltinProperty::LENGTH)) {
    member->type = Type::intTy();
    return this->state.walk.tasks.pushChecked(member);
  }
  bool isWrappedValue = BuiltinPropertyTable::isName(member->name, BuiltinProperty::WRAPPED_VALUE);
  if (type->isInteger() && type->isWrapping() && isWrappedValue) {
    const auto *plain = Type::unsignedInteger(type->bitWidth());
    this->state.walk.tasks.pushChecked(member->object);
    return this->state.walk.tasks.pushTask(Convert{.to = plain});
  }
  if (type->isOptional()) {
    auto message = ErrorTextMemberRead::undefined(member->name);
    return this->reportMember(member, CodeMembersRegistryCheckMemberErrorCode::MAY_BE_UNDEFINED,
                              message);
  }
  bool isRaw = BuiltinPropertyTable::isName(member->name, BuiltinProperty::RAW_TEXTS);
  if (type == Type::array(Type::string()) && isRaw) {
    auto message = ErrorTextMemberRead::raw();
    return this->reportMember(member, CodeMembersRegistryCheckMemberErrorCode::RAW_TEXTS, message);
  }
  if (type->isArray() || type->isString() || type->isIterator()) {
    auto message = ErrorTextMemberRead::mustCall(member->name, type->name());
    return this->reportMember(member, CodeMembersRegistryCheckMemberErrorCode::METHOD_NOT_CALLED,
                              message);
  }
  auto message = ErrorTextMemberRead::noProperty(member->name, type->name());

  return this->reportMember(member, CodeMembersRegistryCheckMemberErrorCode::NO_SUCH_PROPERTY,
                            message);
}

void CodeMembersRegistry::afterEnumResolved(const AfterEnumResolved &task) {
  MemberExpr *member = task.member;
  EnumDecl *enumDecl = task.enumDecl;
  const auto *enumMember = enumDecl->find(member->name);
  if (enumMember == nullptr) {
    auto message = ErrorTextEnumMember::missing(member->name, enumDecl->name);
    member->type = Type::error();
    return this->reportMember(member, CodeMembersRegistryCheckMemberErrorCode::ENUM_MEMBER,
                              message);
  }
  if (enumMember->isString) {
    return this->pushTextMember(enumDecl, member, *enumMember, task.expected);
  }

  return this->pushNumberMember(enumDecl, member, *enumMember, task.expected);
}

void CodeMembersRegistry::pushTextMember(EnumDecl *enumDecl, const MemberExpr *member,
                                         const EnumMember &enumMember, const Type *expected) {
  auto *literalExpr = this->state.ast.make<StringLitExpr>(member->loc(), enumMember.text);
  this->rememberMemberUse(enumDecl, member, literalExpr);
  const auto *literal = this->state.types.literals.stringLiteralFor(expected, enumMember.text);
  literalExpr->type = Type::stringLiteral(enumMember.text);
  this->state.walk.tasks.pushChecked(literalExpr);
  if (literal == nullptr) {
    this->state.walk.tasks.pushTask(Convert{.to = enumDecl->valuesType()});
  }
}

void CodeMembersRegistry::pushNumberMember(const EnumDecl *enumDecl, const MemberExpr *member,
                                           const EnumMember &enumMember, const Type *expected) {
  auto &ast = this->state.ast;
  int64_t value = enumMember.number;
  auto magnitude = static_cast<uint64_t>(value < 0 ? -value : value);
  Expr *size = ast.make<IntLitExpr>(member->loc(), magnitude);
  this->rememberMemberUse(enumDecl, member, size);
  if (value < 0) {
    Expr *negative = ast.make<UnaryExpr>(member->loc(), UnaryOp::Neg, size);
    return this->state.walk.tasks.pushTask(CheckExpr{.expr = negative, .expected = expected});
  }

  return this->state.walk.tasks.pushTask(CheckExpr{.expr = size, .expected = expected});
}

Expr *CodeMembersRegistry::globalConstant(GlobalName object, llvm::StringRef name,
                                          llvm::SMLoc loc) {
  auto value = GlobalConstantTable::find(object, name);
  if (!value.has_value()) {
    return nullptr;
  }
  auto *literal = this->state.ast.make<FloatLitExpr>(loc, *value);
  literal->type = Type::doubleTy();

  return literal;
}

Expr *CodeMembersRegistry::chainLink(Expr *object) {
  const Type *type = object->type;
  if (type->nonOptional()->isJson()) {
    return this->shortCircuitLink(object, Type::json());
  }
  if (!type->isOptional()) {
    return object;
  }

  return this->shortCircuitLink(object, type->inner());
}

Expr *CodeMembersRegistry::shortCircuitLink(Expr *object, const Type *type) {
  auto *link = this->state.ast.make<CastExpr>(object->loc(), object, std::nullopt);
  link->type = type;
  link->shortCircuit = true;
  this->state.walk.functions.markShortCircuit();

  return link;
}

std::string CodeMembersRegistry::globalThisMessage(llvm::StringRef name) const {
  if (GlobalNameTable::find(name) == GlobalName::CRYPTO) {
    return ErrorTextMemberRead::crypto();
  }

  return ErrorTextMemberRead::globalThis(name);
}

std::optional<CodeMembersRegistry::ResultMemberReadable>
CodeMembersRegistry::constantObjectMember(MemberExpr *member) {
  for (GlobalName global : kConstantHolders) {
    if (this->state.isGlobalName(member->object, global)) {
      return this->globalConstantMember(member, global);
    }
  }

  return std::nullopt;
}

CodeMembersRegistry::ResultMemberReadable
CodeMembersRegistry::globalConstantMember(MemberExpr *member, GlobalName global) {
  auto *constant = this->globalConstant(global, member->name, member->loc());
  if (constant != nullptr) {
    return ok(constant);
  }
  bool isNumberFunction = NumberStaticTable::find(member->name).has_value();
  if (global == GlobalName::NUMBER && !isNumberFunction) {
    auto message = ErrorTextMemberRead::numberProp(member->name);

    return err(CodeMembersRegistryCheckMemberError{
        .code = CodeMembersRegistryCheckMemberErrorCode::NUMBER_PROPERTY,
        .message = message,
    });
  }
  auto globalName = GlobalNameTable::getName(global);
  auto message = ErrorTextMemberRead::asValue(globalName, member->name);

  return err(CodeMembersRegistryCheckMemberError{
      .code = CodeMembersRegistryCheckMemberErrorCode::GLOBAL_FUNCTION,
      .message = message,
  });
}

void CodeMembersRegistry::rememberMemberUse(const EnumDecl *enumDecl, const MemberExpr *member,
                                            const Expr *value) {
  bool isWritten = member->object != nullptr && member->object->loc() != member->loc();
  if (!isWritten) {
    return;
  }
  auto named = NamedUse{
      .decl = enumDecl,
      .name = member->object->loc(),
      .member = member->name,
  };
  this->state.inventory.namedUses[value] = named;
}

}
