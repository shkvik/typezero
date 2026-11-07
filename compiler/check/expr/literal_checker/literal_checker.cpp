#include "compiler/check/expr/literal_checker/literal_checker.h"
#include "compiler/check/expr/concat_checker/concat_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/types/literal_types/literal_types.h"
#include "compiler/check/types/type_operators/type_operators.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

LiteralChecker::LiteralChecker(Checker &owner)
    : checker(owner) {}

bool LiteralChecker::fitsIn(const IntLitExpr *lit, const Type *type, bool negated) {
  if (negated) {
    return lit->value == 0 ||
           (!type->isUnsigned() && lit->value <= uint64_t(0) - uint64_t(type->intMin()));
  }

  return lit->value <= type->intMax();
}

LiteralChecker::ResultIntLiteralTyped
LiteralChecker::checkIntLit(IntLitExpr *lit, const Type *expected, bool negated) {
  if (expected) {
    double value = negated ? -double(lit->value) : double(lit->value);
    if (const Type *literal = this->checker.literalTypes().numberLiteralFor(expected, value, false);
        literal && (!negated || value != 0)) {
      lit->type = literal;
      return ok(lit);
    }
    if (const Type *member = LiteralTypes::numberMemberOf(expected, false)) {
      expected = member;
    }
    expected = expected->nonOptional();
  }
  bool exact = expected && (expected->isNumeric() || expected->isDecimal());
  bool tooBig = exact && expected->isInteger() && !LiteralChecker::fitsIn(lit, expected, negated);
  if (exact) {
    lit->type = expected;
  } else if (LiteralChecker::fitsIn(lit, Type::intTy(), negated)) {
    lit->type = Type::intTy();
  } else {
    lit->type = Type::doubleTy();
  }
  std::pair<const char *, unsigned> key{lit->loc().getPointer(), unsigned(Expr::Kind::IntLit)};
  if (!this->checker.functions_.empty() && this->checker.functions_.front()->sym &&
      this->checker.functions_.front()->sym->isGeneric()) {
    this->checker.forced_[key] = lit->type;
  } else if (this->checker.checkingInstance_) {
    if (const Type *type = this->checker.forced_.lookup(key);
        type && TypeRelations::isImplicitlyConvertible(lit->type, type)) {
      lit->type = type;
    }
  }
  if (tooBig) {
    std::string message = ErrorTextIntLit::tooBig(expected->name());

    return err(LiteralCheckerCheckIntLitError{
        .code = LiteralCheckerCheckIntLitErrorCode::DOES_NOT_FIT,
        .message = message,
    });
  }

  return ok(lit);
}

Expr *LiteralChecker::checkTemplate(TemplateExpr *tmpl, const Type *expected) {
  std::vector<const Type *> parts;
  bool constant = true;
  for (Expr *&e : tmpl->exprs) {
    e = this->checker.exprChecker().checkExpr(e);
    const Expr *part = e;
    if (auto *conv = dyn_cast<CastExpr>(e); conv && conv->isImplicit() && conv->type->isString()) {
      part = conv->operand;
    }
    parts.push_back(part->type);
    constant &= isa<NameExpr>(part) || isa<StringLitExpr>(part) || isa<IntLitExpr>(part) ||
                isa<BoolLitExpr>(part);
    e = this->checker.concatChecker().toStringOperand(e);
  }
  tmpl->type = Type::string();
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
      this->checker.typeOperators().evalTypeOp(Type::typeOp(TypeOp::Template, operands), operands);
  if (type->isPattern() ||
      (type->typeOp() == TypeOp::Template && TypeRelations::isImplicitlyConvertible(type, want))) {
    tmpl->type = type;
  } else if (type->isStringLiteral() && constant) {
    auto *literal = this->checker.ctx_.make<StringLitExpr>(tmpl->loc(), type->literalText());
    literal->type = type;
    return literal;
  }

  return tmpl;
}

}
