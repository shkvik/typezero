#include "compiler/semantic/bodies/expr/concat_checker/concat_checker.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/builtin_names/type_name_table/type_name_table.h"
#include "compiler/program/library/object/object_member_table/object_member_table.h"
#include "compiler/semantic/bodies/shared_helpers/shared_helpers.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/generics/generic_checker/generic_checker.h"
#include "compiler/semantic/library/bytes/bytes_checker/bytes_checker.h"
#include "compiler/semantic/library/date/date_checker/date_checker.h"
#include "compiler/semantic/library/json/json_checker/json_checker.h"
#include "compiler/semantic/library/regex/regex_checker/regex_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/unknown_checker/unknown_checker.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

constexpr const char *kUnnamedObject = "o";
constexpr size_t kOperatorLength = 1;
constexpr size_t kTemplateFrame = 4;

}

ConcatChecker::ConcatChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->dateChecker = &all.dateChecker;
  this->jsonChecker = &all.jsonChecker;
  this->bytesChecker = &all.bytesChecker;
  this->regexChecker = &all.regexChecker;
  this->typeRelations = &all.typeRelations;
  this->classDeclarer = &all.classDeclarer;
  this->genericChecker = &all.genericChecker;
  this->unknownChecker = &all.unknownChecker;
}

const Expr *ConcatChecker::unconverted(const Expr *expr) const {
  auto *castExpr = dyn_cast<CastExpr>(expr);
  bool isTextCast = castExpr != nullptr && castExpr->isImplicit() && castExpr->type->isString();

  return isTextCast ? castExpr->operand : expr;
}

ConcatChecker::Parts ConcatChecker::concatParts(const BinaryExpr *binary) const {
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

const BinaryExpr *ConcatChecker::leftConcat(const BinaryExpr *binary) const {
  const auto *inner = this->unconverted(binary->lhs);
  auto *left = dyn_cast<BinaryExpr>(inner);
  bool isConcat = left != nullptr && left->op == BinaryOp::Add && left->type != nullptr &&
                  left->type->isString();

  return isConcat ? left : nullptr;
}

ConcatChecker::TemplateText ConcatChecker::concatTemplate(const Parts &parts) const {
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

std::string ConcatChecker::escapedBody(llvm::StringRef text) const {
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

std::string ConcatChecker::templateChar(llvm::StringRef body, size_t index) const {
  char letter = body[index];
  bool isOpening = letter == '$' && body.substr(index + 1).starts_with("{");
  bool isSpecial = letter == '`' || isOpening;
  auto shown = std::string(1, letter);

  return isSpecial ? "\\" + shown : shown;
}

ConcatChecker::ResultConvertsToText ConcatChecker::checkConvertible(const Expr *expr) const {
  auto problem = this->textProblem(expr);
  if (!problem.has_value()) {
    return ok(expr);
  }
  auto message = this->textProblemMessage(expr, *problem);

  return err(ConcatCheckerConvertibleError{
      .code = *problem,
      .message = message,
  });
}

std::optional<ConcatCheckerConvertibleErrorCode>
ConcatChecker::textProblem(const Expr *expr) const {
  const Type *type = expr->type;
  const Type *base = type->nonOptional();
  if (type->isVoid()) {
    return ConcatCheckerConvertibleErrorCode::VOID_VALUE;
  }
  if (type->holdsFunctions()) {
    return ConcatCheckerConvertibleErrorCode::FUNCTION_VALUE;
  }
  if (base->isTuple()) {
    return ConcatCheckerConvertibleErrorCode::TUPLE_VALUE;
  }
  if (type->holdsResults()) {
    return ConcatCheckerConvertibleErrorCode::RESULT_VALUE;
  }
  if (this->hasOwnToString(base)) {
    return ConcatCheckerConvertibleErrorCode::OBJECT_TO_STRING;
  }
  if (base->isDict()) {
    return ConcatCheckerConvertibleErrorCode::DICT_VALUE;
  }
  if (base->isIterator()) {
    return ConcatCheckerConvertibleErrorCode::ITERATOR_VALUE;
  }
  if (this->unknownChecker->isHoldingUnknown(type)) {
    return ConcatCheckerConvertibleErrorCode::UNKNOWN_VALUE;
  }
  if (this->jsonChecker->holdsJson(type)) {
    return ConcatCheckerConvertibleErrorCode::JSON_VALUE;
  }
  if (type->holdsObjects()) {
    return ConcatCheckerConvertibleErrorCode::OBJECT_VALUE;
  }
  if (this->heldUnionMember(base) != nullptr) {
    return ConcatCheckerConvertibleErrorCode::UNION_MEMBER;
  }

  return std::nullopt;
}

bool ConcatChecker::hasOwnToString(const Type *base) const {
  auto *classDecl = this->classDeclarer->classOf(base);
  if (classDecl == nullptr) {
    return false;
  }
  auto toStringName = ObjectMemberTable::getName(ObjectMember::TO_STRING);

  return !this->classDeclarer->methodsNamed(classDecl, toStringName).empty();
}

const Type *ConcatChecker::heldUnionMember(const Type *base) const {
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

bool ConcatChecker::isHeldMember(const Type *member) const {
  bool isPlainObject = member->isObject() && !member->isDate() && !member->isRegExp();

  return isPlainObject || member->isDict() || member->isFunction() || member->isIterator();
}

std::string ConcatChecker::textProblemMessage(const Expr *expr,
                                              ConcatCheckerConvertibleErrorCode problem) const {
  const Type *type = expr->type;
  const Type *base = type->nonOptional();
  switch (problem) {
    case ConcatCheckerConvertibleErrorCode::VOID_VALUE:
      return ErrorTextToString::voidValue();
    case ConcatCheckerConvertibleErrorCode::FUNCTION_VALUE:
      return ErrorTextToString::function();
    case ConcatCheckerConvertibleErrorCode::TUPLE_VALUE:
      return ErrorTextToString::tuple();
    case ConcatCheckerConvertibleErrorCode::RESULT_VALUE:
      return ErrorTextToString::result();
    case ConcatCheckerConvertibleErrorCode::OBJECT_TO_STRING:
      return this->ownStringMessage(expr);
    case ConcatCheckerConvertibleErrorCode::DICT_VALUE:
      return this->dictMessage(base);
    case ConcatCheckerConvertibleErrorCode::ITERATOR_VALUE:
      return ErrorTextToString::iterator();
    case ConcatCheckerConvertibleErrorCode::UNKNOWN_VALUE:
      return ErrorTextToString::unknown();
    case ConcatCheckerConvertibleErrorCode::JSON_VALUE:
      return ErrorTextToString::json();
    case ConcatCheckerConvertibleErrorCode::OBJECT_VALUE:
      return ErrorTextToString::object();
    case ConcatCheckerConvertibleErrorCode::UNION_MEMBER:
      return this->unionMemberMessage(type, base);
  }

  return ErrorTextToString::object();
}

std::string ConcatChecker::unionMemberMessage(const Type *type, const Type *base) const {
  const auto *member = this->heldUnionMember(base);

  return ErrorTextToString::member(type->name(), member->name());
}

std::string ConcatChecker::ownStringMessage(const Expr *expr) const {
  auto *name = dyn_cast<NameExpr>(expr);
  std::string text = name != nullptr ? name->name.str() : kUnnamedObject;

  return ErrorTextToString::ownString(text);
}

std::string ConcatChecker::dictMessage(const Type *base) const {
  auto kind = this->dictKind(base);
  auto shown = base->isRecord() ? GlobalNameTable::getName(GlobalName::OBJECT) : kind;

  return ErrorTextToString::dict(kind, shown);
}

llvm::StringRef ConcatChecker::dictKind(const Type *base) const {
  if (base->isRecord()) {
    return TypeNameTable::getName(TypeName::RECORD);
  }
  if (base->isMap()) {
    return TypeNameTable::getName(TypeName::MAP);
  }

  return TypeNameTable::getName(TypeName::SET);
}

Expr *ConcatChecker::toStringOperand(Expr *expr) {
  const Type *type = expr->type;
  if (type->isString() || type->isError()) {
    return expr;
  }
  if (type->isRegExp()) {
    return this->regexChecker->regexToString(expr);
  }
  if (type->isDate()) {
    return this->dateChecker->dateToString(expr);
  }
  if (type->isBytes()) {
    return this->bytesChecker->bytesToString(expr);
  }
  if (type->nonOptional()->isParam()) {
    this->genericChecker->noteForced(expr, Type::string());
    return this->typeRelations->convert(expr, Type::string());
  }
  auto convertible = this->checkConvertible(expr);
  if (convertible.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), convertible.error().message);
    return expr;
  }
  const Type *base = type->nonOptional();
  if (base->isUnion() && base->isTagged()) {
    return this->taggedText(expr);
  }

  return this->typeRelations->convert(expr, Type::string());
}

Expr *ConcatChecker::taggedText(Expr *expr) {
  auto *converted = this->typeRelations->convert(expr, Type::string());
  auto *castExpr = dyn_cast<CastExpr>(converted);
  if (castExpr == nullptr) {
    return expr;
  }
  castExpr->text = true;

  return castExpr;
}

std::string ConcatChecker::mixedConcatText(const BinaryExpr *root, const BinaryExpr *mixed) const {
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

std::string ConcatChecker::sumHint(const Parts &parts) const {
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

bool ConcatChecker::isTextValue(const Expr *expr) const {
  return expr->type->isString() && !isa<StringLitExpr>(expr) && !isa<TemplateExpr>(expr);
}

}
