#include "compiler/semantic/code/concatenation/code_concatenation_registry.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/builtin_names/type_name_table/type_name_table.h"
#include "compiler/program/library/builtin_names/type_of_name_table/type_of_name_table.h"
#include "compiler/program/library/object/object_member_table/object_member_table.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/program/types/type_spelling/type_spelling.h"
#include "compiler/semantic/bodies/shared_helpers/shared_helpers.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

constexpr const char *kUnnamedObject = "o";
constexpr size_t kOperatorLength = 1;
constexpr size_t kTemplateFrame = 4;

}

CodeConcatenationRegistry::CodeConcatenationRegistry(SemanticState &owner)
    : state(owner) {}

void CodeConcatenationRegistry::trackMixedConcat(const TrackMixedConcat &task) {
  BinaryExpr *binary = task.binary;
  auto &concatenation = this->state.code.concatenation;
  const Type *leftType = binary->lhs->type;
  const Type *rightType = binary->rhs->type;
  bool isMixing = (leftType->isString() && this->isJoiningAsText(rightType)) ||
                  (rightType->isString() && this->isJoiningAsText(leftType));
  if (concatenation.getMixedConcat() == nullptr && isMixing) {
    concatenation.rememberMixedConcat(binary);
  }
  if (!task.isChainEnd) {
    return;
  }
  const auto *first = concatenation.getMixedConcat();
  if (first != nullptr) {
    auto mixed = this->mixedConcatText(binary, first);
    this->state.diagnostics.error(first->loc(), mixed);
  }
  concatenation.rememberMixedConcat(task.outerMixed);
}

void CodeConcatenationRegistry::checkConcatenation(const CheckConcatenation &task) {
  BinaryExpr *binary = task.binary;
  binary->type = Type::string();
  this->state.walk.tasks.pushChecked(binary);
  this->state.walk.tasks.pushTask(ToStringSlot{.slot = &binary->rhs});

  return this->state.walk.tasks.pushTask(ToStringSlot{.slot = &binary->lhs});
}

void CodeConcatenationRegistry::toStringSlot(const ToStringSlot &task) {
  this->state.walk.tasks.pushChecked(*task.slot);
  this->state.walk.tasks.pushTask(WriteCheckedSlot{.slot = task.slot});

  return this->state.walk.tasks.pushTask(ToStringOperand{});
}

void CodeConcatenationRegistry::toStringOperand() {
  Expr *expr = this->state.walk.tasks.takeChecked();
  this->state.walk.tasks.pushChecked(expr);
  const Type *type = expr->type;
  if (type->isString() || type->isError()) {
    return;
  }
  if (type->isRegExp()) {
    return this->state.walk.tasks.pushTask(RegexToString{});
  }
  if (type->isDate()) {
    return this->state.walk.tasks.pushTask(DateToString{});
  }
  if (type->isBytes()) {
    return this->state.walk.tasks.pushTask(BytesToString{});
  }
  if (type->nonOptional()->isParam()) {
    const Expr *bare = ExprParts::withoutImplicitCasts(expr);
    this->state.generics.copies.rememberForcedType(bare, Type::string());
    return this->state.walk.tasks.pushTask(Convert{.to = Type::string()});
  }
  auto convertible = this->checkConvertible(expr);
  if (convertible.isErr()) {
    return this->state.diagnostics.error(expr->loc(), convertible.error().message);
  }
  const Type *base = type->nonOptional();
  if (base->isUnion() && base->isTagged()) {
    this->state.walk.tasks.pushTask(MarkTextCast{});
  }

  return this->state.walk.tasks.pushTask(Convert{.to = Type::string()});
}

void CodeConcatenationRegistry::markTextCast() {
  Expr *converted = this->state.walk.tasks.takeChecked();
  auto *castExpr = dyn_cast<CastExpr>(converted);
  if (castExpr != nullptr) {
    castExpr->text = true;
  }

  return this->state.walk.tasks.pushChecked(converted);
}

bool CodeConcatenationRegistry::isHoldingUnknown(const Type *type) const {
  const Type *current = type;
  while (current->isOptional() || current->isArray()) {
    current = current->inner();
  }

  return current->isUnknown();
}

bool CodeConcatenationRegistry::isHoldingJson(const Type *type) const {
  const Type *current = type;
  while (current->isOptional() || current->isArray()) {
    current = current->inner();
  }

  return current->isJson();
}

bool CodeConcatenationRegistry::isJoiningAsText(const Type *type) const {
  const Type *base = type->nonOptional();

  return base->isNumeric() || base->isDecimal() || base->isBool();
}

const Expr *CodeConcatenationRegistry::unconverted(const Expr *expr) const {
  auto *castExpr = dyn_cast<CastExpr>(expr);
  bool isTextCast = castExpr != nullptr && castExpr->isImplicit() && castExpr->type->isString();

  return isTextCast ? castExpr->operand : expr;
}

CodeConcatenationRegistry::Parts
CodeConcatenationRegistry::concatParts(const BinaryExpr *binary) const {
  auto chain = std::vector<const BinaryExpr *>();
  const BinaryExpr *link = binary;
  while (link != nullptr) {
    chain.push_back(link);
    link = this->leftConcat(link);
  }
  auto parts = Parts();
  const BinaryExpr *first = chain.back();
  auto firstTexts = SharedHelpers::operandTexts(first->source, first->loc(), kOperatorLength);
  parts.push_back({this->unconverted(first->lhs), firstTexts.first});
  for (const BinaryExpr *step : llvm::reverse(chain)) {
    auto texts = SharedHelpers::operandTexts(step->source, step->loc(), kOperatorLength);
    parts.push_back({this->unconverted(step->rhs), texts.second});
  }

  return parts;
}

const BinaryExpr *CodeConcatenationRegistry::leftConcat(const BinaryExpr *binary) const {
  const auto *inner = this->unconverted(binary->lhs);
  auto *left = dyn_cast<BinaryExpr>(inner);
  bool isConcat = left != nullptr && left->op == BinaryOp::Add && left->type != nullptr &&
                  left->type->isString();

  return isConcat ? left : nullptr;
}

CodeConcatenationRegistry::TemplateText
CodeConcatenationRegistry::concatTemplate(const Parts &parts) const {
  auto result = std::string("`");
  unsigned values = 0;
  llvm::StringRef only;
  for (auto [expr, text] : parts) {
    if (text.empty()) {
      return TemplateText{};
    }
    bool isQuoted = text.size() >= 2 && text.back() == text.front();
    bool isQuotedString =
        isa<StringLitExpr>(expr) && isQuoted && (text.front() == '"' || text.front() == '\'');
    bool isQuotedTemplate = isa<TemplateExpr>(expr) && isQuoted && text.front() == '`';
    if (isQuotedString) {
      result += this->escapedBody(text);
    } else if (isQuotedTemplate) {
      result += text.drop_front().drop_back();
    } else {
      only = SharedHelpers::withoutParens(text);
      ++values;
      result += ("${" + only + "}").str();
    }
  }
  bool isSingleValue = values == 1 && result.size() == only.size() + kTemplateFrame;

  return TemplateText{
      .text = result + "`",
      .only = isSingleValue ? only : llvm::StringRef(),
  };
}

std::string CodeConcatenationRegistry::escapedBody(llvm::StringRef text) const {
  llvm::StringRef body = text.drop_front().drop_back();
  auto escaped = std::string();
  size_t index = 0;
  while (index < body.size()) {
    bool isEscape = body[index] == '\\' && index + 1 < body.size();
    if (isEscape) {
      escaped += body.substr(index, 2);
      index += 2;
    } else {
      escaped += this->templateChar(body, index);
      index += 1;
    }
  }

  return escaped;
}

std::string CodeConcatenationRegistry::templateChar(llvm::StringRef body, size_t index) const {
  char letter = body[index];
  bool isOpening = letter == '$' && body.substr(index + 1).starts_with("{");
  bool isSpecial = letter == '`' || isOpening;
  auto shown = std::string(1, letter);

  return isSpecial ? "\\" + shown : shown;
}

CodeConcatenationRegistry::ResultConvertsToText
CodeConcatenationRegistry::checkConvertible(const Expr *expr) const {
  auto problem = this->textProblem(expr);
  if (!problem.has_value()) {
    return ok(expr);
  }
  auto message = this->textProblemMessage(expr, *problem);

  return err(CodeConcatenationRegistryConvertibleError{
      .code = *problem,
      .message = message,
  });
}

std::optional<CodeConcatenationRegistryConvertibleErrorCode>
CodeConcatenationRegistry::textProblem(const Expr *expr) const {
  const Type *type = expr->type;
  const Type *base = type->nonOptional();
  if (type->isVoid()) {
    return CodeConcatenationRegistryConvertibleErrorCode::VOID_VALUE;
  }
  if (type->holdsFunctions()) {
    return CodeConcatenationRegistryConvertibleErrorCode::FUNCTION_VALUE;
  }
  if (base->isTuple()) {
    return CodeConcatenationRegistryConvertibleErrorCode::TUPLE_VALUE;
  }
  if (type->holdsResults()) {
    return CodeConcatenationRegistryConvertibleErrorCode::RESULT_VALUE;
  }
  if (this->hasOwnToString(base)) {
    return CodeConcatenationRegistryConvertibleErrorCode::OBJECT_TO_STRING;
  }
  if (base->isDict()) {
    return CodeConcatenationRegistryConvertibleErrorCode::DICT_VALUE;
  }
  if (base->isIterator()) {
    return CodeConcatenationRegistryConvertibleErrorCode::ITERATOR_VALUE;
  }
  if (this->isHoldingUnknown(type)) {
    return CodeConcatenationRegistryConvertibleErrorCode::UNKNOWN_VALUE;
  }
  if (this->isHoldingJson(type)) {
    return CodeConcatenationRegistryConvertibleErrorCode::JSON_VALUE;
  }
  if (type->holdsObjects()) {
    return CodeConcatenationRegistryConvertibleErrorCode::OBJECT_VALUE;
  }
  if (this->heldUnionMember(base) != nullptr) {
    return CodeConcatenationRegistryConvertibleErrorCode::UNION_MEMBER;
  }

  return std::nullopt;
}

bool CodeConcatenationRegistry::hasOwnToString(const Type *base) const {
  auto *classDecl = this->classOf(base);
  if (classDecl == nullptr) {
    return false;
  }
  auto toStringName = ObjectMemberTable::getName(ObjectMember::TO_STRING);

  return !classDecl->findMethodsNamed(toStringName).empty();
}

const Type *CodeConcatenationRegistry::heldUnionMember(const Type *base) const {
  if (!base->isUnion() || !base->isTagged()) {
    return nullptr;
  }
  for (const Type *member : base->members()) {
    if (this->isHeldMember(member)) {
      return member;
    }
  }

  return nullptr;
}

bool CodeConcatenationRegistry::isHeldMember(const Type *member) const {
  bool isPlainObject = member->isObject() && !member->isDate() && !member->isRegExp();

  return isPlainObject || member->isDict() || member->isFunction() || member->isIterator();
}

std::string CodeConcatenationRegistry::textProblemMessage(
    const Expr *expr, CodeConcatenationRegistryConvertibleErrorCode problem) const {
  const Type *type = expr->type;
  const Type *base = type->nonOptional();
  switch (problem) {
    case CodeConcatenationRegistryConvertibleErrorCode::VOID_VALUE:
      return ErrorTextToString::voidValue();
    case CodeConcatenationRegistryConvertibleErrorCode::FUNCTION_VALUE:
      return ErrorTextToString::function();
    case CodeConcatenationRegistryConvertibleErrorCode::TUPLE_VALUE:
      return ErrorTextToString::tuple();
    case CodeConcatenationRegistryConvertibleErrorCode::RESULT_VALUE:
      return ErrorTextToString::result();
    case CodeConcatenationRegistryConvertibleErrorCode::OBJECT_TO_STRING:
      return this->ownStringMessage(expr);
    case CodeConcatenationRegistryConvertibleErrorCode::DICT_VALUE:
      return this->dictMessage(base);
    case CodeConcatenationRegistryConvertibleErrorCode::ITERATOR_VALUE:
      return ErrorTextToString::iterator();
    case CodeConcatenationRegistryConvertibleErrorCode::UNKNOWN_VALUE:
      return ErrorTextToString::unknown();
    case CodeConcatenationRegistryConvertibleErrorCode::JSON_VALUE:
      return ErrorTextToString::json();
    case CodeConcatenationRegistryConvertibleErrorCode::OBJECT_VALUE:
      return ErrorTextToString::object();
    case CodeConcatenationRegistryConvertibleErrorCode::UNION_MEMBER:
      return this->unionMemberMessage(type, base);
  }

  return ErrorTextToString::object();
}

std::string CodeConcatenationRegistry::unionMemberMessage(const Type *type,
                                                          const Type *base) const {
  const auto *member = this->heldUnionMember(base);

  return ErrorTextToString::member(type->name(), member->name());
}

std::string CodeConcatenationRegistry::ownStringMessage(const Expr *expr) const {
  auto *name = dyn_cast<NameExpr>(expr);
  std::string text = name != nullptr ? name->name.str() : kUnnamedObject;

  return ErrorTextToString::ownString(text);
}

std::string CodeConcatenationRegistry::dictMessage(const Type *base) const {
  auto kind = this->dictKind(base);
  auto shown = base->isRecord() ? GlobalNameTable::getName(GlobalName::OBJECT) : kind;

  return ErrorTextToString::dict(kind, shown);
}

llvm::StringRef CodeConcatenationRegistry::dictKind(const Type *base) const {
  if (base->isRecord()) {
    return TypeNameTable::getName(TypeName::RECORD);
  }
  if (base->isMap()) {
    return TypeNameTable::getName(TypeName::MAP);
  }

  return TypeNameTable::getName(TypeName::SET);
}

std::string CodeConcatenationRegistry::mixedConcatText(const BinaryExpr *root,
                                                       const BinaryExpr *mixed) const {
  const Type *leftType = this->unconverted(mixed->lhs)->type;
  const Type *rightType = this->unconverted(mixed->rhs)->type;
  auto operands = ErrorTextMixedConcat::operands(leftType->name(), rightType->name());
  auto parts = this->concatParts(root);
  auto joined = this->concatTemplate(parts);
  if (joined.text.empty()) {
    return operands + ErrorTextMixedConcat::literal();
  }
  if (!joined.only.empty()) {
    return operands + ErrorTextMixedConcat::convert(joined.only);
  }
  auto sum = this->sumHint(parts);
  if (!sum.empty()) {
    return operands + ErrorTextMixedConcat::addOrJoin(sum, joined.text);
  }

  return operands + ErrorTextMixedConcat::asTemplate(joined.text);
}

std::string CodeConcatenationRegistry::sumHint(const Parts &parts) const {
  if (parts.size() != 2) {
    return "";
  }
  auto [first, firstText] = parts[0];
  auto [second, secondText] = parts[1];
  if (first->type->isNumeric() && this->isTextValue(second)) {
    return ErrorTextMixedConcat::addRight(firstText, secondText);
  }
  if (this->isTextValue(first) && second->type->isNumeric()) {
    return ErrorTextMixedConcat::addLeft(firstText, secondText);
  }

  return "";
}

bool CodeConcatenationRegistry::isTextValue(const Expr *expr) const {
  return expr->type->isString() && !isa<StringLitExpr>(expr) && !isa<TemplateExpr>(expr);
}

ClassDecl *CodeConcatenationRegistry::classOf(const Type *type) const {
  if (type == nullptr) {
    return nullptr;
  }
  const auto *classDecl = type->classDecl();

  return const_cast<ClassDecl *>(classDecl);
}

}
