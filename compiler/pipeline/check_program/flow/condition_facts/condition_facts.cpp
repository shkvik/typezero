#include "compiler/pipeline/check_program/flow/condition_facts/condition_facts.h"
#include "compiler/load/to_utf8/to_utf8.h"
#include "compiler/pipeline/check_program/flow/field_facts/field_facts.h"
#include "compiler/pipeline/check_program/flow/index_proofs/index_proofs.h"
#include "compiler/pipeline/check_program/flow/predicate_checker/predicate_checker.h"
#include "compiler/pipeline/check_program/flow/union_narrowing/union_narrowing.h"
#include "compiler/pipeline/check_program/generics/generic_checker/generic_checker.h"
#include "compiler/pipeline/check_program/topics/json/json_checker/json_checker.h"
#include "compiler/pipeline/check_program/topics/result/result_checker/result_checker.h"
#include "compiler/pipeline/check_program/types/unknown_checker/unknown_checker.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

const VarSymbol *ConditionFacts::variableOf(const Expr *expr) {
  if (auto *conv = dyn_cast<CastExpr>(expr);
      conv && conv->isImplicit() && conv->operand->type->isOptional()) {
    expr = conv->operand;
  }
  auto *name = dyn_cast<NameExpr>(expr);

  return name ? name->var() : nullptr;
}

const NameExpr *ConditionFacts::optionalVariable(const Expr *expr) {
  if (auto *assign = dyn_cast<AssignExpr>(expr); assign && !assign->op) {
    expr = assign->target;
  }
  auto *name = dyn_cast<NameExpr>(expr);

  return name && name->var() && name->var()->type->isOptional() ? name : nullptr;
}

void ConditionFacts::addLess(Facts &facts, const Linear &x, const Linear &y, bool orEqual) {
  facts.add(x.term, y.term, y.offset - x.offset + (orEqual ? 1 : 0));
}

ConditionFacts::ConditionFacts(CheckState &owner)
    : state(owner) {}

bool ConditionFacts::chainDefined(const Expr *e, Flow &flow) const {
  while (auto *conv = dyn_cast<CastExpr>(e)) {
    if (!conv->isImplicit()) {
      break;
    }
    e = conv->operand;
  }
  auto *chain = dyn_cast<OptionalChainExpr>(e);
  if (!chain) {
    return false;
  }
  std::function<std::optional<FieldPath>(const Expr *)> walk =
      [&](const Expr *x) -> std::optional<FieldPath> {
    if (auto *link = dyn_cast<CastExpr>(x)) {
      if (!link->shortCircuit && !link->isImplicit()) {
        return std::nullopt;
      }
      std::optional<FieldPath> inner = walk(link->operand);
      if (link->shortCircuit && inner) {
        if (inner->names.empty()) {
          flow.defined.insert(inner->var);
        } else {
          FieldFacts::defineField(flow, *inner);
        }
      }
      return inner;
    }
    if (auto *name = dyn_cast<NameExpr>(x)) {
      if (!name->var()) {
        return std::nullopt;
      }
      FieldPath path;
      path.var = name->var();
      return path;
    }
    if (auto *member = dyn_cast<MemberExpr>(x)) {
      std::optional<FieldPath> path = walk(member->object);
      if (path) {
        path->names.push_back(member->name);
      }
      return path;
    }
    return std::nullopt;
  };
  std::optional<FieldPath> whole = walk(chain->chain);
  if (whole && !whole->names.empty()) {
    FieldFacts::defineField(flow, *whole);
  }

  return true;
}

void ConditionFacts::addConditionFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const {
  ResultChecker(this->state).addResultFacts(cond, whenTrue, whenFalse);
  JsonChecker(this->state).addJsonFacts(cond, whenTrue, whenFalse);
  UnionNarrowing(this->state).addUnionFacts(cond, whenTrue, whenFalse);
  UnknownChecker(this->state).addUnknownFacts(cond, whenTrue, whenFalse);
  if (auto *call = dyn_cast<CallExpr>(cond); call && call->builtin() == Builtin::None) {
    PredicateChecker(this->state).applyPredicate(call, whenTrue, whenFalse);
    return;
  }
  if (const NameExpr *name = ConditionFacts::optionalVariable(cond)) {
    whenTrue.defined.insert(name->var());
    return;
  }
  auto optionalField = [&](const Expr *expr) -> std::optional<FieldPath> {
    if (!expr->type || !expr->type->isOptional()) {
      return std::nullopt;
    }

    return FieldFacts(this->state).fieldPath(expr);
  };
  if (std::optional<FieldPath> path = optionalField(cond)) {
    FieldFacts::defineField(whenTrue, *path);
    return;
  }
  if (this->chainDefined(cond, whenTrue)) {
    return;
  }
  auto *bin = dyn_cast<BinaryExpr>(cond);
  if (!bin) {
    return;
  }
  BinaryOp op = bin->op;
  if (op == BinaryOp::Eq || op == BinaryOp::Ne) {
    for (auto [side, other] : {std::pair{bin->lhs, bin->rhs}, std::pair{bin->rhs, bin->lhs}}) {
      const Expr *given = other;
      while (auto *conv = dyn_cast<CastExpr>(given)) {
        if (!conv->isImplicit() || conv->shortCircuit) {
          break;
        }
        given = conv->operand;
      }
      if (isa<UndefinedExpr>(given)) {
        this->chainDefined(side, op == BinaryOp::Ne ? whenTrue : whenFalse);
      } else if (given->type && !given->type->isOptional() && !given->type->isUndefined()) {
        this->chainDefined(side, op == BinaryOp::Eq ? whenTrue : whenFalse);
      }
    }
  }

  if (op == BinaryOp::InstanceOf) {
    if (const NameExpr *name = ConditionFacts::optionalVariable(bin->lhs)) {
      whenTrue.defined.insert(name->var());
    } else if (std::optional<FieldPath> path = optionalField(bin->lhs)) {
      FieldFacts::defineField(whenTrue, *path);
    }
    const Type *cls = bin->rhs->type;
    if (cls && cls->isConstructor()) {
      cls = cls->constructed();
    }
    if (const VarSymbol *var = ConditionFacts::variableOf(bin->lhs);
        var && cls && cls->isClass() && var->type->nonOptional()->isObject() &&
        cls != var->type->nonOptional() && cls->isSubtypeOf(var->type->nonOptional())) {
      whenTrue.narrowed.push_back({var, cls});
    }
    if (const VarSymbol *var = ConditionFacts::variableOf(bin->lhs);
        var && cls && cls->isBuffer() && var->type->nonOptional()->bytes() == Type::Bytes::Uint8) {
      whenTrue.narrowed.push_back({var, cls});
    }
    return;
  }

  if (op == BinaryOp::Eq || op == BinaryOp::Ne) {
    if (isa<UndefinedExpr>(bin->lhs) || isa<UndefinedExpr>(bin->rhs)) {
      const Expr *side = isa<UndefinedExpr>(bin->rhs) ? bin->lhs : bin->rhs;
      if (const NameExpr *name = ConditionFacts::optionalVariable(side)) {
        (op == BinaryOp::Ne ? whenTrue : whenFalse).defined.insert(name->var());
        (op == BinaryOp::Ne ? whenFalse : whenTrue).undefinedVars.insert(name->var());
      } else if (std::optional<FieldPath> path = optionalField(side)) {
        FieldFacts::defineField(op == BinaryOp::Ne ? whenTrue : whenFalse, *path);
      }
      return;
    }
    for (auto [side, other] : {std::pair{bin->lhs, bin->rhs}, std::pair{bin->rhs, bin->lhs}}) {
      auto *conv = dyn_cast<CastExpr>(other);
      const NameExpr *name = ConditionFacts::optionalVariable(side);
      if (name && conv && conv->isImplicit() && !conv->operand->type->isOptional() &&
          !conv->operand->type->isUndefined()) {
        (op == BinaryOp::Eq ? whenTrue : whenFalse).defined.insert(name->var());
      }
    }
    for (auto [side, other] : {std::pair{bin->lhs, bin->rhs}, std::pair{bin->rhs, bin->lhs}}) {
      auto *typeOf = dyn_cast<UnaryExpr>(side);
      auto *text = dyn_cast<StringLitExpr>(other);
      if (!typeOf || typeOf->op != UnaryOp::TypeOf || !text) {
        continue;
      }
      if (const VarSymbol *var = ConditionFacts::variableOf(typeOf->operand);
          var && var->type->nonOptional()->isParam()) {
        const Type *param = var->type->nonOptional();
        auto admits = [&](auto test) {
          llvm::ArrayRef<const Type *> alternatives = param->alternatives();
          const Type *constraint = param->constraint();

          return constraint ? test(constraint->nonOptional())
                            : alternatives.empty() || llvm::any_of(alternatives, test);
        };
        const Type *narrowed = nullptr;
        std::string wanted = ToUtf8::toUtf8(text->value);
        auto numberNamed = [&](llvm::StringRef name) -> const Type * {
          for (const Type *t : param->alternatives()) {
            if (t->isNumeric() && name == TypeSpelling::typeOfName(t)) {
              return GenericChecker(this->state).numberParam(param, t);
            }
          }
          const Type *named = UnknownChecker::typeOfNarrowed(std::u16string(name.begin(), name.end()));
          if (param->alternatives().empty() && named && named->isNumeric()) {
            return GenericChecker(this->state).numberParam(param, named);
          }
          return nullptr;
        };
        if (text->value == u"string" && admits([](const Type *t) { return t->isString(); })) {
          narrowed = Type::string();
        } else if (text->value == u"boolean" && admits([](const Type *t) { return t->isBool(); })) {
          narrowed = Type::boolTy();
        } else if (!param->constraint()) {
          narrowed = numberNamed(wanted);
        }
        Flow &holds = op == BinaryOp::Eq ? whenTrue : whenFalse;
        if (narrowed) {
          holds.narrowed.push_back({var, narrowed});
          if (var->type->isOptional()) {
            holds.defined.insert(var);
          }
        }
        llvm::SmallVector<const Type *, 4> others;
        for (const Type *t : param->alternatives()) {
          llvm::StringRef name = TypeSpelling::typeOfName(t);
          if (std::u16string(name.begin(), name.end()) != text->value) {
            others.push_back(t);
          }
        }
        if (!others.empty() && !var->type->isOptional()) {
          llvm::StringRef kind = TypeSpelling::typeOfName(others.front());
          bool same = llvm::all_of(
              others, [&](const Type *t) { return TypeSpelling::typeOfName(t) == kind; });
          const Type *rest = !same               ? nullptr
                             : kind == "string"  ? Type::string()
                             : kind == "boolean" ? Type::boolTy()
                                                 : numberNamed(kind);
          if (rest) {
            (op == BinaryOp::Eq ? whenFalse : whenTrue).narrowed.push_back({var, rest});
          }
        }
        continue;
      }
      const NameExpr *name = ConditionFacts::optionalVariable(typeOf->operand);
      if (!name) {
        continue;
      }
      if (text->value == u"undefined") {
        (op == BinaryOp::Eq ? whenFalse : whenTrue).defined.insert(name->var());
        (op == BinaryOp::Eq ? whenTrue : whenFalse).undefinedVars.insert(name->var());
      } else {
        (op == BinaryOp::Eq ? whenTrue : whenFalse).defined.insert(name->var());
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
  std::optional<Linear> l = IndexProofs(this->state).linear(bin->lhs),
                        r = IndexProofs(this->state).linear(bin->rhs);
  if (!l || !r) {
    return;
  }
  Facts &t = whenTrue.facts;
  Facts &f = whenFalse.facts;
  if (l->term.isZero() && r->term.isZero()) {
    int64_t a = l->offset;
    int64_t b = r->offset;
    bool holds = op == BinaryOp::Lt   ? a < b
                 : op == BinaryOp::Le ? a <= b
                 : op == BinaryOp::Gt ? a > b
                 : op == BinaryOp::Ge ? a >= b
                 : op == BinaryOp::Eq ? a == b
                                      : a != b;
    (holds ? whenFalse : whenTrue).unreachable = true;
    return;
  }

  if ((op == BinaryOp::Ne || op == BinaryOp::Eq) && (l->term.isZero() || r->term.isZero())) {
    const Linear &x = l->term.isZero() ? *r : *l;
    const Linear &c = l->term.isZero() ? *l : *r;
    Facts &differ = op == BinaryOp::Ne ? t : f;
    if (this->state.getFlowState().getFacts().proves(c.term, x.term, x.offset - c.offset + 1)) {
      ConditionFacts::addLess(differ, c, x, false);
    } else if (this->state.getFlowState().getFacts().proves(x.term, c.term,
                                                            c.offset - x.offset + 1)) {
      ConditionFacts::addLess(differ, x, c, false);
    }
  }
  switch (op) {
  case BinaryOp::Lt:
    ConditionFacts::addLess(t, *l, *r, false);
    ConditionFacts::addLess(f, *r, *l, true);
    break;
  case BinaryOp::Le:
    ConditionFacts::addLess(t, *l, *r, true);
    ConditionFacts::addLess(f, *r, *l, false);
    break;
  case BinaryOp::Gt:
    ConditionFacts::addLess(t, *r, *l, false);
    ConditionFacts::addLess(f, *l, *r, true);
    break;
  case BinaryOp::Ge:
    ConditionFacts::addLess(t, *r, *l, true);
    ConditionFacts::addLess(f, *l, *r, false);
    break;
  case BinaryOp::Eq:
    ConditionFacts::addLess(t, *l, *r, true);
    ConditionFacts::addLess(t, *r, *l, true);
    break;
  case BinaryOp::Ne:
    ConditionFacts::addLess(f, *l, *r, true);
    ConditionFacts::addLess(f, *r, *l, true);
    break;
  default:
    break;
  }
}

}
