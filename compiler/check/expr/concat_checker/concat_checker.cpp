#include "compiler/check/expr/concat_checker/concat_checker.h"
#include "compiler/check/generics/generic_checker/generic_checker.h"
#include "compiler/check/shared_helpers/shared_helpers.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/types/unknown_checker/unknown_checker.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/library/bytes/bytes_checker/bytes_checker.h"
#include "compiler/library/date/date_checker/date_checker.h"
#include "compiler/library/json/json_checker/json_checker.h"
#include "compiler/library/regex/regex_checker/regex_checker.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

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
  auto [lhs, rhs] = ConcatChecker::operandTexts(bin->source, bin->loc(), 1);
  auto *left = dyn_cast<BinaryExpr>(ConcatChecker::unconverted(bin->lhs));
  if (left && left->op == BinaryOp::Add && left->type && left->type->isString()) {
    ConcatChecker::concatParts(left, parts);
  } else {
    parts.push_back({ConcatChecker::unconverted(bin->lhs), lhs});
  }
  parts.push_back({ConcatChecker::unconverted(bin->rhs), rhs});
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
      only = withoutParens(text);
      ++values;
      result += ("${" + only + "}").str();
    }
  }
  if (values != 1 || result.size() != only.size() + 4) {
    only = "";
  }

  return result + "`";
}

ConcatChecker::ConcatChecker(Checker &owner)
    : checker(owner) {}

ConcatChecker::ResultConvertsToText ConcatChecker::convertible(const Expr *expr) {
  const Type *type = expr->type;
  const Type *base = type->nonOptional();
  if (type->isVoid()) {
    std::string message = ErrorTextToString::voidValue();

    return err(ConcatCheckerConvertibleError{
        .code = ConcatCheckerConvertibleErrorCode::VOID_VALUE,
        .message = message,
    });
  }
  if (Checker::holdsFunctions(type)) {
    std::string message = ErrorTextToString::function();

    return err(ConcatCheckerConvertibleError{
        .code = ConcatCheckerConvertibleErrorCode::FUNCTION_VALUE,
        .message = message,
    });
  }
  if (base->isTuple()) {
    std::string message = ErrorTextToString::tuple();

    return err(ConcatCheckerConvertibleError{
        .code = ConcatCheckerConvertibleErrorCode::TUPLE_VALUE,
        .message = message,
    });
  }
  if (Checker::holdsResults(type)) {
    std::string message = ErrorTextToString::result();

    return err(ConcatCheckerConvertibleError{
        .code = ConcatCheckerConvertibleErrorCode::RESULT_VALUE,
        .message = message,
    });
  }
  ClassDecl *cls = ClassDeclarer::classOf(base);
  bool ownToString = cls && !ClassDeclarer::methodsNamed(cls, "toString", false).empty();
  if (ownToString) {
    auto *name = dyn_cast<NameExpr>(expr);
    std::string text = name ? name->name.str() : "o";
    std::string message = ErrorTextToString::ownString(text);

    return err(ConcatCheckerConvertibleError{
        .code = ConcatCheckerConvertibleErrorCode::OBJECT_TO_STRING,
        .message = message,
    });
  }
  if (base->isDict()) {
    const char *kind = base->isRecord() ? "Record" : base->isMap() ? "Map" : "Set";
    const char *shown = base->isRecord() ? "Object" : kind;
    std::string message = ErrorTextToString::dict(kind, shown);

    return err(ConcatCheckerConvertibleError{
        .code = ConcatCheckerConvertibleErrorCode::DICT_VALUE,
        .message = message,
    });
  }
  if (base->isIterator()) {
    std::string message = ErrorTextToString::iterator();

    return err(ConcatCheckerConvertibleError{
        .code = ConcatCheckerConvertibleErrorCode::ITERATOR_VALUE,
        .message = message,
    });
  }
  if (UnknownChecker::holdsUnknown(type)) {
    std::string message = ErrorTextToString::unknown();

    return err(ConcatCheckerConvertibleError{
        .code = ConcatCheckerConvertibleErrorCode::UNKNOWN_VALUE,
        .message = message,
    });
  }
  if (JsonChecker::holdsJson(type)) {
    std::string message = ErrorTextToString::json();

    return err(ConcatCheckerConvertibleError{
        .code = ConcatCheckerConvertibleErrorCode::JSON_VALUE,
        .message = message,
    });
  }
  if (Checker::holdsObjects(type)) {
    std::string message = ErrorTextToString::object();

    return err(ConcatCheckerConvertibleError{
        .code = ConcatCheckerConvertibleErrorCode::OBJECT_VALUE,
        .message = message,
    });
  }
  bool tagged = base->isUnion() && base->isTagged();
  for (const Type *m : tagged ? base->members() : llvm::ArrayRef<const Type *>()) {
    bool held = (m->isObject() && !m->isDate() && !m->isRegExp()) || m->isDict() ||
                m->isFunction() || m->isIterator();
    if (held) {
      std::string message = ErrorTextToString::member(type->name(), m->name());

      return err(ConcatCheckerConvertibleError{
          .code = ConcatCheckerConvertibleErrorCode::UNION_MEMBER,
          .message = message,
      });
    }
  }

  return ok(expr);
}

Expr *ConcatChecker::toStringOperand(Expr *expr) {
  const Type *type = expr->type;
  if (type->isString() || type->isError()) {
    return expr;
  }
  if (type->isRegExp()) {
    return this->checker.regexChecker().regexToString(expr);
  }
  if (type->isDate()) {
    return this->checker.dateChecker().dateToString(expr);
  }
  if (type->isBytes()) {
    return this->checker.bytesChecker().bytesToString(expr);
  }
  if (type->nonOptional()->isParam()) {
    this->checker.genericChecker().noteForced(expr, Type::string());
    return this->checker.typeRelations().convert(expr, Type::string());
  }
  ResultConvertsToText convertible = ConcatChecker::convertible(expr);
  if (convertible.isErr()) {
    this->checker.diags_.error(expr->loc(), convertible.error().message);
    return expr;
  }
  if (const Type *u = type->nonOptional(); u->isUnion() && u->isTagged()) {
    auto *cast = dyn_cast<CastExpr>(this->checker.typeRelations().convert(expr, Type::string()));
    if (cast) {
      cast->text = true;
    }
    return cast ? cast : expr;
  }

  return this->checker.typeRelations().convert(expr, Type::string());
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
