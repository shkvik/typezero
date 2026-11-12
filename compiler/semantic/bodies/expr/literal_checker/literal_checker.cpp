#include "compiler/semantic/bodies/expr/literal_checker/literal_checker.h"
#include "compiler/semantic/bodies/expr/concat_checker/concat_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/literal_types/literal_types.h"
#include "compiler/semantic/types/type_operators/type_operators.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

LiteralChecker::LiteralChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->exprChecker = &all.exprChecker;
  this->literalTypes = &all.literalTypes;
  this->concatChecker = &all.concatChecker;
  this->typeOperators = &all.typeOperators;
}

bool LiteralChecker::isFitting(const IntLitExpr *literal, const Type *type,
                               LiteralSign sign) const {
  if (sign == LiteralSign::PLAIN) {
    return literal->value <= type->intMax();
  }
  uint64_t magnitude = uint64_t(0) - uint64_t(type->intMin());

  return literal->value == 0 || (!type->isUnsigned() && literal->value <= magnitude);
}

LiteralChecker::ResultIntLiteralTyped
LiteralChecker::checkIntLit(IntLitExpr *literalExpr, const Type *expected, LiteralSign sign) {
  const auto *literal = this->literalTypeFor(literalExpr, expected, sign);
  if (literal != nullptr) {
    literalExpr->type = literal;
    return ok(literalExpr);
  }
  const auto *target = this->numberTarget(expected);
  bool isExact = target != nullptr && (target->isNumeric() || target->isDecimal());
  bool isTooBig = isExact && target->isInteger() && !this->isFitting(literalExpr, target, sign);
  if (isExact) {
    literalExpr->type = target;
  } else if (this->isFitting(literalExpr, Type::intTy(), sign)) {
    literalExpr->type = Type::intTy();
  } else {
    literalExpr->type = Type::doubleTy();
  }
  this->rememberForcedType(literalExpr);
  if (isTooBig) {
    auto message = ErrorTextIntLit::tooBig(target->name());

    return err(LiteralCheckerCheckIntLitError{
        .code = LiteralCheckerCheckIntLitErrorCode::DOES_NOT_FIT,
        .message = message,
    });
  }

  return ok(literalExpr);
}

const Type *LiteralChecker::literalTypeFor(const IntLitExpr *literalExpr, const Type *expected,
                                           LiteralSign sign) const {
  if (expected == nullptr) {
    return nullptr;
  }
  bool isNegated = sign == LiteralSign::NEGATED;
  auto magnitude = double(literalExpr->value);
  double value = isNegated ? -magnitude : magnitude;
  const auto *literal =
      this->literalTypes->numberLiteralFor(expected, value, NumberSpelling::WHOLE);
  if (literal == nullptr || (isNegated && value == 0)) {
    return nullptr;
  }

  return literal;
}

const Type *LiteralChecker::numberTarget(const Type *expected) const {
  if (expected == nullptr) {
    return nullptr;
  }
  const auto *member = this->literalTypes->numberMemberOf(expected, NumberSpelling::WHOLE);
  const Type *chosen = member != nullptr ? member : expected;

  return chosen->nonOptional();
}

void LiteralChecker::rememberForcedType(IntLitExpr *literalExpr) {
  auto &generics = this->state.getGenericStore();
  if (this->isInsideGeneric()) {
    generics.rememberForcedType(literalExpr, literalExpr->type);
    return;
  }
  if (!generics.isCheckingInstance()) {
    return;
  }
  const auto *type = generics.findForcedType(literalExpr);
  if (type != nullptr && TypeRelations::isImplicitlyConvertible(literalExpr->type, type)) {
    literalExpr->type = type;
  }
}

bool LiteralChecker::isInsideGeneric() const {
  auto &functions = this->state.getFunctionStack();
  if (!functions.isInsideFunction()) {
    return false;
  }
  const FuncSymbol *outermost = functions.getOutermostDecl()->sym;

  return outermost != nullptr && outermost->isGeneric();
}

Expr *LiteralChecker::checkTemplate(TemplateExpr *templateExpr, const Type *expected) {
  auto parts = std::vector<const Type *>();
  bool isConstant = true;
  for (Expr *&expr : templateExpr->exprs) {
    expr = this->exprChecker->checkExpr(expr);
    const auto *part = this->templatePart(expr);
    parts.push_back(part->type);
    isConstant = isConstant && this->isConstantPart(part);
    expr = this->concatChecker->toStringOperand(expr);
  }
  templateExpr->type = Type::string();
  const Type *want = expected != nullptr ? expected->nonOptional() : nullptr;
  if (want == nullptr || !this->isWantingTemplate(want)) {
    return templateExpr;
  }
  auto operands = this->templateOperands(templateExpr, parts);
  if (!operands.has_value()) {
    return templateExpr;
  }

  return this->typedTemplate(templateExpr, *operands, want, isConstant);
}

const Expr *LiteralChecker::templatePart(const Expr *expr) const {
  auto *conversion = dyn_cast<CastExpr>(expr);
  bool isTextCast =
      conversion != nullptr && conversion->isImplicit() && conversion->type->isString();

  return isTextCast ? conversion->operand : expr;
}

bool LiteralChecker::isConstantPart(const Expr *part) const {
  return isa<NameExpr>(part) || isa<StringLitExpr>(part) || isa<IntLitExpr>(part) ||
         isa<BoolLitExpr>(part);
}

bool LiteralChecker::isTemplated(const Type *candidate) const {
  return candidate->isPattern() || candidate->typeOp() == TypeOp::Template ||
         candidate->isStringLiteral();
}

bool LiteralChecker::isWantingTemplate(const Type *want) const {
  if (this->isTemplated(want)) {
    return true;
  }
  if (!want->isUnion()) {
    return false;
  }
  for (const Type *member : want->members()) {
    if (this->isTemplated(member)) {
      return true;
    }
  }

  return false;
}

bool LiteralChecker::isTemplatePartType(const Type *part) const {
  return part->isString() || part->isNumeric() || part->isBool() || part->isLiteral() ||
         part->isParam();
}

std::optional<std::vector<const Type *>>
LiteralChecker::templateOperands(const TemplateExpr *templateExpr,
                                 llvm::ArrayRef<const Type *> parts) const {
  auto operands = std::vector<const Type *>();
  for (size_t index = 0; index < templateExpr->quasis.size(); ++index) {
    operands.push_back(Type::stringLiteral(templateExpr->quasis[index]));
    bool hasPart = index < parts.size();
    if (hasPart && !this->isTemplatePartType(parts[index])) {
      return std::nullopt;
    }
    if (hasPart) {
      operands.push_back(parts[index]);
    }
  }

  return operands;
}

Expr *LiteralChecker::typedTemplate(TemplateExpr *templateExpr,
                                    const std::vector<const Type *> &operands, const Type *want,
                                    bool isConstant) {
  const auto *unevaluated = Type::typeOp(TypeOp::Template, operands);
  const auto *type = this->typeOperators->evalTypeOp(unevaluated, operands);
  bool isFittingTemplate =
      type->typeOp() == TypeOp::Template && TypeRelations::isImplicitlyConvertible(type, want);
  if (type->isPattern() || isFittingTemplate) {
    templateExpr->type = type;
    return templateExpr;
  }
  if (!type->isStringLiteral() || !isConstant) {
    return templateExpr;
  }
  auto *literal =
      this->state.getAst().make<StringLitExpr>(templateExpr->loc(), type->literalText());
  literal->type = type;

  return literal;
}

}
