#include "compiler/check/generics/generic_checker/generic_checker.h"
#include "compiler/check/expr/concat_checker/concat_checker.h"
#include "compiler/check/generics/generic_check_helpers/generic_check_helpers.h"
#include "compiler/check/generics/inference/inference.h"
#include "compiler/check/scopes/scopes.h"
#include "compiler/check/shared_helpers/shared_helpers.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>
#include <set>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

const Type *literalType(const Expr *value);

bool typeMentions(const Type *type, llvm::ArrayRef<const Type *> params) {
  return mentionsAnyOf(type, params);
}

GenericChecker::GenericChecker(Checker &checker) : checker_(checker) {}

const Type *GenericChecker::apparentType(const Type *type) {
  if (type->isParam() && type->constraint() && !type->isListParam()) {
    return type->constraint();
  }
  if (type->isLiteral()) {
    return type->literalBase();
  }
  if (type->isUnion() && llvm::all_of(type->members(), [&](const Type *m) {
        return m->isLiteral() && m->literalBase() == type->members().front()->literalBase();
      })) {
    return type->members().front()->literalBase();
  }
  if (type->isUnion() && llvm::any_of(type->members(), [](const Type *m) {
        return m->isParam() && m->constraint() && !m->isListParam();
      })) {
    std::vector<const Type *> members;
    for (const Type *m : type->members()) {
      bool bounded = m->isParam() && m->constraint() && !m->isListParam();
      const Type *seen = bounded ? m->constraint() : m;
      if (!seen->isObject() || seen->isClass()) {
        return type;
      }
      members.push_back(seen);
    }
    const Type *joined = Type::unionOf(members);

    return joined->isTagged() ? type : joined;
  }

  return type;
}

Expr *GenericChecker::apparent(Expr *expr) {
  const Type *type = apparentType(expr->type);

  return type == expr->type ? expr : checker_.typeRelations().convert(expr, type);
}

FuncSymbol *GenericChecker::genericNamed(const NameExpr *name) const {
  Checker::ScopeEntry entry = checker_.scopes().lookup(name->name);

  return entry.func && !entry.var && entry.func->isGeneric() ? entry.func : nullptr;
}

const Type *GenericChecker::numberParam(const Type *param) const {
  const Type *&number = numberParams_[param];
  if (!number) {
    const Type *constraint = nullptr;
    for (const Type *t : param->alternatives()) {
      if (t->isNumeric()) {
        constraint = constraint ? TypeRelations::commonNumericType(constraint, t) : t;
      }
    }
    Type *type = Type::param(param->name());
    type->setConstraint(constraint ? constraint : Type::doubleTy(), {});
    number = type;
  }

  return number;
}

const Type *GenericChecker::narrowedType(const VarSymbol *var) const {
  for (const auto &[narrowed, type] : checker_.flow_.narrowed) {
    if (narrowed == var) {
      return type;
    }
  }

  return nullptr;
}

void GenericChecker::noteForced(const Expr *expr, const Type *to) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit()) {
      break;
    }
    expr = conv->operand;
  }
  checker_.forced_[{expr->loc().getPointer(), unsigned(expr->kind())}] = to;
}

Expr *GenericChecker::forceConversion(Expr *expr, const Type *to) {
  const Type *type = expr->type;
  if (!type || type->isError() || type == to) {
    return expr;
  }
  if (to->isString()) {
    return checker_.concatChecker().toStringOperand(expr);
  }
  if (TypeRelations::isImplicitlyConvertible(type, to)) {
    return checker_.typeRelations().convert(expr, to);
  }

  return expr;
}

std::optional<bool> GenericChecker::constantCondition(const Expr *cond) const {
  while (auto *conv = dyn_cast<CastExpr>(cond)) {
    if (!conv->isImplicit() || !conv->type->isBool() || !conv->operand->type->isBool()) {
      break;
    }
    cond = conv->operand;
  }
  if (auto *lit = dyn_cast<BoolLitExpr>(cond)) {
    return lit->value;
  }
  if (auto *unary = dyn_cast<UnaryExpr>(cond); unary && unary->op == UnaryOp::Not) {
    if (std::optional<bool> value = constantCondition(unary->operand)) {
      return !*value;
    }
    return std::nullopt;
  }
  auto *bin = dyn_cast<BinaryExpr>(cond);
  if (!bin) {
    return std::nullopt;
  }
  if (bin->op == BinaryOp::LogAnd || bin->op == BinaryOp::LogOr) {
    bool isAnd = bin->op == BinaryOp::LogAnd;
    std::optional<bool> l = constantCondition(bin->lhs);
    if (l && *l != isAnd) {
      return l;
    }
    if (l) {
      return constantCondition(bin->rhs);
    }
    return std::nullopt;
  }
  if (bin->op != BinaryOp::Eq && bin->op != BinaryOp::Ne) {
    return std::nullopt;
  }
  for (auto [side, other] : {std::pair{bin->lhs, bin->rhs}, std::pair{bin->rhs, bin->lhs}}) {
    const Expr *operand = side;
    while (isa<CastExpr>(operand) && cast<CastExpr>(operand)->isImplicit()) {
      operand = cast<CastExpr>(operand)->operand;
    }
    const Type *type = operand->type;
    if (isa<UndefinedExpr>(other) && isa<NameExpr>(operand) &&
        (type->isUndefined() || (type->isOptional() && type->inner()->isNever()))) {
      return bin->op == BinaryOp::Eq;
    }
  }
  for (auto [side, other] : {std::pair{bin->lhs, bin->rhs}, std::pair{bin->rhs, bin->lhs}}) {
    auto *typeOf = dyn_cast<UnaryExpr>(side);
    auto *text = dyn_cast<StringLitExpr>(other);
    if (!typeOf || typeOf->op != UnaryOp::TypeOf || !text) {
      continue;
    }
    const Expr *operand = typeOf->operand;
    while (isa<CastExpr>(operand) && cast<CastExpr>(operand)->isImplicit()) {
      operand = cast<CastExpr>(operand)->operand;
    }
    if (!isa<NameExpr>(operand)) {
      return std::nullopt;
    }
    const Type *type = typeOf->operand->type;
    llvm::StringRef name = typeOfName(type->nonOptional());
    if (name.empty()) {
      return std::nullopt;
    }
    std::u16string wanted(name.begin(), name.end());
    bool equal;
    if (type->isOptional()) {
      if (text->value == u"undefined" || text->value == wanted) {
        return std::nullopt;
      }
      equal = false;
    } else {
      equal = text->value == wanted;
    }
    return bin->op == BinaryOp::Eq ? equal : !equal;
  }

  return std::nullopt;
}

const Type *GenericChecker::literalShape(const Expr *expr) {
  if (const Type *type = literalType(expr)) {
    return isa<TemplateExpr>(expr) ? nullptr : type;
  }
  if (isa<UndefinedExpr>(expr)) {
    return Type::undefinedTy();
  }
  if (auto *lit = dyn_cast<ArrayLitExpr>(expr)) {
    const Type *element = nullptr;
    for (const ArrayLitExpr::Element &e : lit->elements) {
      const Type *type = e.spread || e.hole ? nullptr : literalShape(e.expr);
      element = type && element ? TypeRelations::unify(element, type) : type;
      if (!element) {
        return nullptr;
      }
    }
    return element && !element->isUndefined() ? Type::array(element) : nullptr;
  }
  if (auto *lit = dyn_cast<ObjectLitExpr>(expr)) {
    std::vector<Field> fields;
    for (const ObjectLitExpr::Property &p : lit->props) {
      const Type *type = literalShape(p.value);
      if (!type || type->isUndefined() ||
          llvm::any_of(fields, [&](const Field &f) { return f.name == p.name; })) {
        return nullptr;
      }
      fields.push_back({p.name.str(), type, false});
    }
    return Type::object(std::move(fields));
  }

  return nullptr;
}

}
