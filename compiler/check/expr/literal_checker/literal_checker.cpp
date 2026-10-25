#include "compiler/check/expr/literal_checker/literal_checker.h"
#include "compiler/check/expr/concat_checker/concat_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/types/literal_types/literal_types.h"
#include "compiler/check/types/type_operators/type_operators.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/ErrorHandling.h"
#include <cmath>
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

LiteralChecker::LiteralChecker(Checker &checker) : checker_(checker) {}

// An integer literal takes the numeric type the context expects; otherwise
// int, or long if it does not fit.
Expr *LiteralChecker::checkIntLit(IntLitExpr *lit, const Type *expected, bool negated) {
  auto fits = [&](unsigned bits) {
    uint64_t limit = uint64_t(1) << (bits - 1);

    return negated ? lit->value <= limit : lit->value < limit;
  };

  if (expected) {
    // `0 | 1`, or a union with a number type: that type.
    double value = negated ? -double(lit->value) : double(lit->value);
    if (const Type *literal =
            checker_.literalTypes().numberLiteralFor(expected, value, /*fraction=*/false);
        literal && (!negated || value != 0)) {
      lit->type = literal;
      return lit;
    }
    if (const Type *member = LiteralTypes::numberMemberOf(expected, /*fraction=*/false)) {
      expected = member;
    }
    expected = expected->nonOptional();
  }
  if (expected && (expected->isNumeric() || expected->isDecimal())) {
    lit->type = expected;
    if (expected->isInteger() && !fits(expected->bitWidth())) {
      checker_.diags_.error(lit->loc(),
                            "integer literal does not fit in type '" + expected->name() + "'");
    }
  } else if (fits(32)) {
    lit->type = Type::intTy();
  } else if (fits(64)) {
    lit->type = Type::longTy();
  } else { // only a double holds it, as any number in JavaScript
    lit->type = Type::doubleTy();
  }
  // The type a literal takes in generic code may come from a type parameter
  // (`reduce(f, 0)` over `T extends number` starts from a double); an
  // instance gives it the same.
  std::pair<const char *, unsigned> key{lit->loc().getPointer(), unsigned(Expr::Kind::IntLit)};
  if (!checker_.functions_.empty() && checker_.functions_.front()->sym &&
      checker_.functions_.front()->sym->isGeneric()) {
    checker_.forced_[key] = lit->type;
  } else if (checker_.checkingInstance_) {
    if (const Type *type = checker_.forced_.lookup(key);
        type && TypeRelations::isImplicitlyConvertible(lit->type, type)) {
      lit->type = type;
    }
  }

  return lit;
}

Expr *LiteralChecker::checkTemplate(TemplateExpr *tmpl, const Type *expected) {
  std::vector<const Type *> parts;
  bool constant = true;
  for (Expr *&e : tmpl->exprs) {
    e = checker_.exprChecker().checkExpr(e);
    // A copy of generic code converts a value of a type parameter to a string
    // here, as the generic check did (noteForced): the type it had.
    const Expr *part = e;
    if (auto *conv = dyn_cast<CastExpr>(e); conv && conv->isImplicit() && conv->type->isString()) {
      part = conv->operand;
    }
    parts.push_back(part->type);
    constant &= isa<NameExpr>(part) || isa<StringLitExpr>(part) || isa<IntLitExpr>(part) ||
                isa<BoolLitExpr>(part);
    e = checker_.concatChecker().toStringOperand(e);
  }
  tmpl->type = Type::string();
  // Where a template literal type is expected, the type the parts make, as
  // in TypeScript: `on${x}` of a string x is an `on${string}`. One literal
  // of a template of variables and literals is that string literal itself.
  const Type *want = expected ? expected->nonOptional() : nullptr;
  auto templated = [](const Type *t) {
    return t->isPattern() || t->typeOp() == TypeOp::Template || t->isStringLiteral();
  };
  if (!want ||
      !(templated(want) || (want->isUnion() && llvm::any_of(want->members(), templated)))) {
    return tmpl;
  }
  std::vector<const Type *> operands;
  for (size_t i = 0; i < tmpl->quasis.size(); ++i) {
    operands.push_back(Type::stringLiteral(tmpl->quasis[i]));
    if (i < parts.size()) {
      const Type *part = parts[i];
      if (!(part->isString() || part->isNumeric() || part->isBool() || part->isLiteral() ||
            part->isParam())) {
        return tmpl;
      }
      operands.push_back(part);
    }
  }
  const Type *type =
      checker_.typeOperators().evalTypeOp(Type::typeOp(TypeOp::Template, operands), operands);
  if (type->isPattern() ||
      (type->typeOp() == TypeOp::Template && TypeRelations::isImplicitlyConvertible(type, want))) {
    tmpl->type = type;
  } else if (type->isStringLiteral() && constant) {
    auto *literal = checker_.ctx_.make<StringLitExpr>(tmpl->loc(), type->literalText());
    literal->type = type;
    return literal;
  }

  return tmpl;
}

} // namespace tz
