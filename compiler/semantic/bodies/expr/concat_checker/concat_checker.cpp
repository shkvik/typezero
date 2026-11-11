#include "compiler/semantic/bodies/expr/concat_checker/concat_checker.h"
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

ConcatChecker::ConcatChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->dateChecker = &all.dateChecker;
  this->bytesChecker = &all.bytesChecker;
  this->regexChecker = &all.regexChecker;
  this->typeRelations = &all.typeRelations;
  this->genericChecker = &all.genericChecker;
}

std::pair<llvm::StringRef, llvm::StringRef>
ConcatChecker::operandTexts(llvm::StringRef source, llvm::SMLoc op, size_t length) {
  const char *at = op.getPointer();
  if (source.empty() || at < source.begin() || at + length > source.end()) {
    return {};
  }

  return {llvm::StringRef(source.begin(), static_cast<size_t>(at - source.begin())).trim(),
          llvm::StringRef(at + length, static_cast<size_t>(source.end() - at - length)).trim()};
}

const Expr *ConcatChecker::unconverted(const Expr *expr) {
  auto *cast = dyn_cast<CastExpr>(expr);

  return cast && cast->isImplicit() && cast->type->isString() ? cast->operand : expr;
}

void ConcatChecker::concatParts(const BinaryExpr *bin, Parts &parts) {
  std::vector<const BinaryExpr *> chain;
  const BinaryExpr *link = bin;
  while (link != nullptr) {
    chain.push_back(link);
    link = ConcatChecker::leftConcat(link);
  }
  const BinaryExpr *first = chain.back();
  std::pair<llvm::StringRef, llvm::StringRef> firstTexts =
      ConcatChecker::operandTexts(first->source, first->loc(), 1);
  parts.push_back({ConcatChecker::unconverted(first->lhs), firstTexts.first});
  for (const BinaryExpr *step : llvm::reverse(chain)) {
    std::pair<llvm::StringRef, llvm::StringRef> texts =
        ConcatChecker::operandTexts(step->source, step->loc(), 1);
    parts.push_back({ConcatChecker::unconverted(step->rhs), texts.second});
  }
}

const BinaryExpr *ConcatChecker::leftConcat(const BinaryExpr *bin) {
  auto *left = dyn_cast<BinaryExpr>(ConcatChecker::unconverted(bin->lhs));
  bool concat = left != nullptr && left->op == BinaryOp::Add && left->type != nullptr &&
                left->type->isString();

  return concat ? left : nullptr;
}

std::string ConcatChecker::concatTemplate(const Parts &parts, llvm::StringRef &only) {
  std::string result = "`";
  unsigned values = 0;
  for (auto [expr, text] : parts) {
    if (text.empty()) {
      return "";
    }
    bool quoted = text.size() >= 2 && text.back() == text.front();
    if (isa<StringLitExpr>(expr) && quoted && (text.front() == '"' || text.front() == '\'')) {
      llvm::StringRef body = text.drop_front().drop_back();
      for (size_t i = 0; i < body.size(); ++i) {
        if (body[i] == '\\' && i + 1 < body.size()) {
          result += body.substr(i, 2);
          ++i;
          continue;
        }
        if (body[i] == '`' || (body[i] == '$' && body.substr(i + 1).starts_with("{"))) {
          result += '\\';
        }
        result += body[i];
      }
    } else if (isa<TemplateExpr>(expr) && quoted && text.front() == '`') {
      result += text.drop_front().drop_back();
    } else {
      only = SharedHelpers::withoutParens(text);
      ++values;
      result += ("${" + only + "}").str();
    }
  }
  if (values != 1 || result.size() != only.size() + 4) {
    only = "";
  }

  return result + "`";
}

ConcatChecker::ResultConvertsToText ConcatChecker::convertible(const Expr *expr) {
  std::optional<ConcatCheckerConvertibleErrorCode> problem = ConcatChecker::textProblem(expr);
  if (!problem.has_value()) {
    return ok(expr);
  }
  std::string message = ConcatChecker::textProblemMessage(expr, *problem);

  return err(ConcatCheckerConvertibleError{
      .code = *problem,
      .message = message,
  });
}

std::optional<ConcatCheckerConvertibleErrorCode> ConcatChecker::textProblem(const Expr *expr) {
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
  if (ConcatChecker::hasOwnToString(base)) {
    return ConcatCheckerConvertibleErrorCode::OBJECT_TO_STRING;
  }
  if (base->isDict()) {
    return ConcatCheckerConvertibleErrorCode::DICT_VALUE;
  }
  if (base->isIterator()) {
    return ConcatCheckerConvertibleErrorCode::ITERATOR_VALUE;
  }
  if (UnknownChecker::holdsUnknown(type)) {
    return ConcatCheckerConvertibleErrorCode::UNKNOWN_VALUE;
  }
  if (JsonChecker::holdsJson(type)) {
    return ConcatCheckerConvertibleErrorCode::JSON_VALUE;
  }
  if (type->holdsObjects()) {
    return ConcatCheckerConvertibleErrorCode::OBJECT_VALUE;
  }
  if (ConcatChecker::heldUnionMember(base) != nullptr) {
    return ConcatCheckerConvertibleErrorCode::UNION_MEMBER;
  }

  return std::nullopt;
}

bool ConcatChecker::hasOwnToString(const Type *base) {
  ClassDecl *cls = ClassDeclarer::classOf(base);
  if (cls == nullptr) {
    return false;
  }
  llvm::StringRef toStringName = ObjectMemberTable::getName(ObjectMember::TO_STRING);

  return !ClassDeclarer::methodsNamed(cls, toStringName, false).empty();
}

const Type *ConcatChecker::heldUnionMember(const Type *base) {
  if (!base->isUnion() || !base->isTagged()) {
    return nullptr;
  }
  for (const Type *m : base->members()) {
    bool held = (m->isObject() && !m->isDate() && !m->isRegExp()) || m->isDict() ||
                m->isFunction() || m->isIterator();
    if (held) {
      return m;
    }
  }

  return nullptr;
}

std::string ConcatChecker::textProblemMessage(const Expr *expr,
                                              ConcatCheckerConvertibleErrorCode problem) {
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
      return ConcatChecker::ownStringMessage(expr);
    case ConcatCheckerConvertibleErrorCode::DICT_VALUE:
      return ConcatChecker::dictMessage(base);
    case ConcatCheckerConvertibleErrorCode::ITERATOR_VALUE:
      return ErrorTextToString::iterator();
    case ConcatCheckerConvertibleErrorCode::UNKNOWN_VALUE:
      return ErrorTextToString::unknown();
    case ConcatCheckerConvertibleErrorCode::JSON_VALUE:
      return ErrorTextToString::json();
    case ConcatCheckerConvertibleErrorCode::OBJECT_VALUE:
      return ErrorTextToString::object();
    case ConcatCheckerConvertibleErrorCode::UNION_MEMBER:
      return ErrorTextToString::member(type->name(), ConcatChecker::heldUnionMember(base)->name());
  }

  return ErrorTextToString::object();
}

std::string ConcatChecker::ownStringMessage(const Expr *expr) {
  auto *name = dyn_cast<NameExpr>(expr);
  std::string text = name != nullptr ? name->name.str() : "o";

  return ErrorTextToString::ownString(text);
}

std::string ConcatChecker::dictMessage(const Type *base) {
  const char *kind = base->isRecord() ? "Record" : base->isMap() ? "Map" : "Set";
  const char *shown = base->isRecord() ? "Object" : kind;

  return ErrorTextToString::dict(kind, shown);
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
  ResultConvertsToText convertible = ConcatChecker::convertible(expr);
  if (convertible.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), convertible.error().message);
    return expr;
  }
  if (const Type *u = type->nonOptional(); u->isUnion() && u->isTagged()) {
    auto *cast = dyn_cast<CastExpr>(this->typeRelations->convert(expr, Type::string()));
    if (cast) {
      cast->text = true;
    }
    return cast ? cast : expr;
  }

  return this->typeRelations->convert(expr, Type::string());
}

std::string ConcatChecker::mixedConcatText(const BinaryExpr *root, const BinaryExpr *mixed) {
  const Type *l = ConcatChecker::unconverted(mixed->lhs)->type;
  const Type *r = ConcatChecker::unconverted(mixed->rhs)->type;
  std::string operands = ErrorTextMixedConcat::operands(l->name(), r->name());
  Parts parts;
  ConcatChecker::concatParts(root, parts);
  llvm::StringRef only;
  std::string joined = ConcatChecker::concatTemplate(parts, only);
  if (joined.empty()) {
    return operands + ErrorTextMixedConcat::literal();
  }
  if (!only.empty()) {
    return operands + ErrorTextMixedConcat::convert(only);
  }
  if (parts.size() == 2) {
    auto [first, firstText] = parts[0];
    auto [second, secondText] = parts[1];
    auto isText = [](const Expr *e) {
      return e->type->isString() && !isa<StringLitExpr>(e) && !isa<TemplateExpr>(e);
    };
    std::string sum;
    if (first->type->isNumeric() && isText(second)) {
      sum = ErrorTextMixedConcat::addRight(firstText, secondText);
    } else if (isText(first) && second->type->isNumeric()) {
      sum = ErrorTextMixedConcat::addLeft(firstText, secondText);
    }
    if (!sum.empty()) {
      return operands + ErrorTextMixedConcat::addOrJoin(sum, joined);
    }
  }

  return operands + ErrorTextMixedConcat::asTemplate(joined);
}

}
