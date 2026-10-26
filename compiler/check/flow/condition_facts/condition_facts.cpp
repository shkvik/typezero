#include "compiler/check/flow/condition_facts/condition_facts.h"
#include "compiler/check/flow/field_facts/field_facts.h"
#include "compiler/check/flow/index_proofs/index_proofs.h"
#include "compiler/check/flow/predicate_checker/predicate_checker.h"
#include "compiler/check/flow/union_narrowing/union_narrowing.h"
#include "compiler/check/generics/generic_checker/generic_checker.h"
#include "compiler/check/types/unknown_checker/unknown_checker.h"
#include "compiler/library/json/json_checker/json_checker.h"
#include "compiler/library/result/result_checker/result_checker.h"
#include "llvm/ADT/STLExtras.h"
#include <limits>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

const VarSymbol *variableOf(const Expr *expr) {
  if (auto *conv = dyn_cast<CastExpr>(expr);
      conv && conv->isImplicit() && conv->operand->type->isOptional()) {
    expr = conv->operand;
  }
  auto *name = dyn_cast<NameExpr>(expr);

  return name ? name->var : nullptr;
}

const NameExpr *optionalVariable(const Expr *expr) {
  if (auto *assign = dyn_cast<AssignExpr>(expr); assign && !assign->op) {
    expr = assign->target;
  }
  auto *name = dyn_cast<NameExpr>(expr);

  return name && name->var && name->var->type->isOptional() ? name : nullptr;
}

void addLess(Facts &facts, const Linear &x, const Linear &y, bool orEqual) {
  facts.add(x.term, y.term, y.offset - x.offset + (orEqual ? 1 : 0));
}

}

ConditionFacts::ConditionFacts(Checker &checker) : checker_(checker) {}

void ConditionFacts::addConditionFacts(const Expr *cond, Checker::Flow &whenTrue,
                                       Checker::Flow &whenFalse) const {
  checker_.resultChecker().addResultFacts(cond, whenTrue, whenFalse);
  checker_.jsonChecker().addJsonFacts(cond, whenTrue, whenFalse);
  checker_.unionNarrowing().addUnionFacts(cond, whenTrue, whenFalse);
  checker_.unknownChecker().addUnknownFacts(cond, whenTrue, whenFalse);
  if (auto *call = dyn_cast<CallExpr>(cond); call && call->builtin == Builtin::None) {
    checker_.predicateChecker().applyPredicate(call, whenTrue, whenFalse);
    return;
  }
  if (const NameExpr *name = optionalVariable(cond)) {
    whenTrue.defined.insert(name->var);
    return;
  }
  auto optionalField = [&](const Expr *expr) -> std::optional<Checker::FieldPath> {
    if (!expr->type || !expr->type->isOptional()) {
      return std::nullopt;
    }

    return checker_.fieldFacts().fieldPath(expr);
  };
  if (std::optional<Checker::FieldPath> path = optionalField(cond)) {
    FieldFacts::defineField(whenTrue, *path);
    return;
  }
  auto *bin = dyn_cast<BinaryExpr>(cond);
  if (!bin) {
    return;
  }
  BinaryOp op = bin->op;

  if (op == BinaryOp::InstanceOf) {
    if (const NameExpr *name = optionalVariable(bin->lhs)) {
      whenTrue.defined.insert(name->var);
    } else if (std::optional<Checker::FieldPath> path = optionalField(bin->lhs)) {
      FieldFacts::defineField(whenTrue, *path);
    }
    const Type *cls = bin->rhs->type;
    if (cls && cls->isConstructor()) {
      cls = cls->constructed();
    }
    if (const VarSymbol *var = variableOf(bin->lhs);
        var && cls && cls->isClass() && var->type->nonOptional()->isObject() &&
        cls != var->type->nonOptional() && cls->isSubtypeOf(var->type->nonOptional())) {
      whenTrue.narrowed.push_back({var, cls});
    }
    if (const VarSymbol *var = variableOf(bin->lhs);
        var && cls && cls->isBuffer() && var->type->nonOptional()->bytes() == Type::Bytes::Uint8) {
      whenTrue.narrowed.push_back({var, cls});
    }
    return;
  }

  if (op == BinaryOp::Eq || op == BinaryOp::Ne) {
    if (isa<UndefinedExpr>(bin->lhs) || isa<UndefinedExpr>(bin->rhs)) {
      const Expr *side = isa<UndefinedExpr>(bin->rhs) ? bin->lhs : bin->rhs;
      if (const NameExpr *name = optionalVariable(side)) {
        (op == BinaryOp::Ne ? whenTrue : whenFalse).defined.insert(name->var);
      } else if (std::optional<Checker::FieldPath> path = optionalField(side)) {
        FieldFacts::defineField(op == BinaryOp::Ne ? whenTrue : whenFalse, *path);
      }
      return;
    }
    for (auto [side, other] : {std::pair{bin->lhs, bin->rhs}, std::pair{bin->rhs, bin->lhs}}) {
      auto *conv = dyn_cast<CastExpr>(other);
      const NameExpr *name = optionalVariable(side);
      if (name && conv && conv->isImplicit() && !conv->operand->type->isOptional() &&
          !conv->operand->type->isUndefined()) {
        (op == BinaryOp::Eq ? whenTrue : whenFalse).defined.insert(name->var);
      }
    }
    for (auto [side, other] : {std::pair{bin->lhs, bin->rhs}, std::pair{bin->rhs, bin->lhs}}) {
      auto *typeOf = dyn_cast<UnaryExpr>(side);
      auto *text = dyn_cast<StringLitExpr>(other);
      if (!typeOf || typeOf->op != UnaryOp::TypeOf || !text) {
        continue;
      }
      if (const VarSymbol *var = variableOf(typeOf->operand);
          var && var->type->nonOptional()->isParam()) {
        const Type *param = var->type->nonOptional();
        auto admits = [&](auto test) {
          llvm::ArrayRef<const Type *> alternatives = param->alternatives();
          const Type *constraint = param->constraint();

          return constraint ? test(constraint->nonOptional())
                            : alternatives.empty() || llvm::any_of(alternatives, test);
        };
        const Type *narrowed = nullptr;
        if (text->value == u"string" && admits([](const Type *t) { return t->isString(); })) {
          narrowed = Type::string();
        } else if (text->value == u"boolean" && admits([](const Type *t) { return t->isBool(); })) {
          narrowed = Type::boolTy();
        } else if (text->value == u"number" && !param->constraint() &&
                   admits([](const Type *t) { return t->isNumeric(); })) {
          narrowed = checker_.genericChecker().numberParam(param);
        }
        Checker::Flow &holds = op == BinaryOp::Eq ? whenTrue : whenFalse;
        if (narrowed) {
          holds.narrowed.push_back({var, narrowed});
          if (var->type->isOptional()) {
            holds.defined.insert(var);
          }
        }
        llvm::SmallVector<const Type *, 4> others;
        for (const Type *t : param->alternatives()) {
          llvm::StringRef name = typeOfName(t);
          if (std::u16string(name.begin(), name.end()) != text->value) {
            others.push_back(t);
          }
        }
        if (!others.empty() && !var->type->isOptional()) {
          llvm::StringRef kind = typeOfName(others.front());
          bool same = llvm::all_of(others, [&](const Type *t) { return typeOfName(t) == kind; });
          const Type *rest = !same               ? nullptr
                             : kind == "string"  ? Type::string()
                             : kind == "boolean" ? Type::boolTy()
                             : kind == "number"  ? checker_.genericChecker().numberParam(param)
                                                 : nullptr;
          if (rest) {
            (op == BinaryOp::Eq ? whenFalse : whenTrue).narrowed.push_back({var, rest});
          }
        }
        continue;
      }
      const NameExpr *name = optionalVariable(typeOf->operand);
      if (!name) {
        continue;
      }
      llvm::StringRef valueType = typeOfName(name->var->type->inner());
      if (text->value == u"undefined") {
        (op == BinaryOp::Eq ? whenFalse : whenTrue).defined.insert(name->var);
      } else if (std::u16string(valueType.begin(), valueType.end()) == text->value) {
        (op == BinaryOp::Eq ? whenTrue : whenFalse).defined.insert(name->var);
      }
    }
  }

  switch (op) {
  case BinaryOp::Lt:
  case BinaryOp::Le:
  case BinaryOp::Gt:
  case BinaryOp::Ge:
  case BinaryOp::Eq:
  case BinaryOp::Ne:
    break;
  default:
    return;
  }
  std::optional<Linear> l = checker_.indexProofs().linear(bin->lhs),
                        r = checker_.indexProofs().linear(bin->rhs);
  if (!l || !r) {
    return;
  }
  Facts &t = whenTrue.facts, &f = whenFalse.facts;

  if ((op == BinaryOp::Ne || op == BinaryOp::Eq) && (l->term.isZero() || r->term.isZero())) {
    const Linear &x = l->term.isZero() ? *r : *l, &c = l->term.isZero() ? *l : *r;
    Facts &differ = op == BinaryOp::Ne ? t : f;
    if (checker_.flow_.facts.proves(c.term, x.term, x.offset - c.offset + 1)) {
      addLess(differ, c, x, false);
    } else if (checker_.flow_.facts.proves(x.term, c.term, c.offset - x.offset + 1)) {
      addLess(differ, x, c, false);
    }
  }
  switch (op) {
  case BinaryOp::Lt:
    addLess(t, *l, *r, false);
    addLess(f, *r, *l, true);
    break;
  case BinaryOp::Le:
    addLess(t, *l, *r, true);
    addLess(f, *r, *l, false);
    break;
  case BinaryOp::Gt:
    addLess(t, *r, *l, false);
    addLess(f, *l, *r, true);
    break;
  case BinaryOp::Ge:
    addLess(t, *r, *l, true);
    addLess(f, *l, *r, false);
    break;
  case BinaryOp::Eq:
    addLess(t, *l, *r, true);
    addLess(t, *r, *l, true);
    break;
  case BinaryOp::Ne:
    addLess(f, *l, *r, true);
    addLess(f, *r, *l, true);
    break;
  default:
    break;
  }
}

}
