#include "compiler/semantic/code/literals/scalar_literals/scalar_literals.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

ScalarLiterals::ScalarLiterals(SemanticState &owner)
    : state(owner) {}

void ScalarLiterals::checkIntLiteral(const CheckIntLiteral &task) {
  auto typed = this->checkIntLit(task.literal, task.expected, task.sign);
  this->state.walk.tasks.pushChecked(task.literal);
  if (typed.isErr()) {
    return this->state.diagnostics.error(task.literal->loc(), typed.error().message);
  }
}

void ScalarLiterals::checkTemplate(const CheckTemplate &task) {
  auto draft = TemplateDraft{
      .templateExpr = task.templateExpr,
      .expected = task.expected,
      .parts = {},
      .isConstant = true,
      .index = 0,
  };

  return this->nextTemplatePart(NextTemplatePart{.draft = std::move(draft)});
}

void ScalarLiterals::nextTemplatePart(const NextTemplatePart &task) {
  TemplateExpr *templateExpr = task.draft.templateExpr;
  size_t index = task.draft.index;
  if (index == templateExpr->exprs.size()) {
    return this->finishTemplateParts(task.draft);
  }
  this->state.walk.tasks.pushTask(AfterTemplatePart{.draft = task.draft});

  return this->state.walk.tasks.pushTask(
      CheckExprSlot{.slot = &templateExpr->exprs[index], .expected = nullptr});
}

void ScalarLiterals::afterTemplatePart(const AfterTemplatePart &task) {
  auto draft = task.draft;
  Expr **slot = &draft.templateExpr->exprs[draft.index];
  const auto *part = this->templatePart(*slot);
  draft.parts.push_back(part->type);
  draft.isConstant = draft.isConstant && this->isConstantPart(part);
  draft.index += 1;
  this->state.walk.tasks.pushTask(NextTemplatePart{.draft = std::move(draft)});

  return this->state.walk.tasks.pushTask(ToStringSlot{.slot = slot});
}

void ScalarLiterals::finishTemplateParts(const TemplateDraft &draft) {
  TemplateExpr *templateExpr = draft.templateExpr;
  templateExpr->type = Type::string();
  const Type *want = draft.expected != nullptr ? draft.expected->nonOptional() : nullptr;
  if (want == nullptr || !this->isWantingTemplate(want)) {
    return this->state.walk.tasks.pushChecked(templateExpr);
  }
  auto operands = this->templateOperands(templateExpr, draft.parts);
  if (!operands.has_value()) {
    return this->state.walk.tasks.pushChecked(templateExpr);
  }
  const auto *unevaluated = Type::typeOp(TypeOp::Template, *operands);
  this->state.walk.tasks.pushTask(AfterTemplateType{
      .templateExpr = templateExpr,
      .want = want,
      .isConstant = draft.isConstant,
  });

  return this->state.walk.tasks.pushTask(
      EvalTypeOp{.op = unevaluated, .operands = std::move(*operands)});
}

void ScalarLiterals::afterTemplateType(const AfterTemplateType &task) {
  TemplateExpr *templateExpr = task.templateExpr;
  const Type *type = this->state.walk.tasks.takeAnswer();
  bool isFittingTemplate = type->typeOp() == TypeOp::Template &&
                           this->state.types.relations.isImplicitlyConvertible(type, task.want);
  if (type->isPattern() || isFittingTemplate) {
    templateExpr->type = type;
    return this->state.walk.tasks.pushChecked(templateExpr);
  }
  if (!type->isStringLiteral() || !task.isConstant) {
    return this->state.walk.tasks.pushChecked(templateExpr);
  }
  auto *literal = this->state.ast.make<StringLitExpr>(templateExpr->loc(), type->literalText());
  literal->type = type;

  return this->state.walk.tasks.pushChecked(literal);
}

bool ScalarLiterals::isFitting(const IntLitExpr *literal, const Type *type,
                               LiteralSign sign) const {
  if (sign == LiteralSign::PLAIN) {
    return literal->value <= type->intMax();
  }
  uint64_t magnitude = uint64_t(0) - uint64_t(type->intMin());

  return literal->value == 0 || (!type->isUnsigned() && literal->value <= magnitude);
}

ScalarLiterals::ResultIntLiteralTyped
ScalarLiterals::checkIntLit(IntLitExpr *literalExpr, const Type *expected, LiteralSign sign) {
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

    return err(CodeLiteralsRegistryCheckIntLitError{
        .code = CodeLiteralsRegistryCheckIntLitErrorCode::DOES_NOT_FIT,
        .message = message,
    });
  }

  return ok(literalExpr);
}

const Type *ScalarLiterals::literalTypeFor(const IntLitExpr *literalExpr, const Type *expected,
                                           LiteralSign sign) const {
  if (expected == nullptr) {
    return nullptr;
  }
  bool isNegated = sign == LiteralSign::NEGATED;
  auto magnitude = double(literalExpr->value);
  double value = isNegated ? -magnitude : magnitude;
  const auto *literal =
      this->state.types.literals.numberLiteralFor(expected, value, NumberSpelling::WHOLE);
  if (literal == nullptr || (isNegated && value == 0)) {
    return nullptr;
  }

  return literal;
}

const Type *ScalarLiterals::numberTarget(const Type *expected) const {
  if (expected == nullptr) {
    return nullptr;
  }
  const auto *member = this->state.types.literals.numberMemberOf(expected, NumberSpelling::WHOLE);
  const Type *chosen = member != nullptr ? member : expected;

  return chosen->nonOptional();
}

void ScalarLiterals::rememberForcedType(IntLitExpr *literalExpr) {
  auto &generics = this->state.generics.copies;
  if (this->isInsideGeneric()) {
    generics.rememberForcedType(literalExpr, literalExpr->type);
    return;
  }
  if (!generics.isCheckingInstance()) {
    return;
  }
  const auto *type = generics.findForcedType(literalExpr);
  if (type != nullptr &&
      this->state.types.relations.isImplicitlyConvertible(literalExpr->type, type)) {
    literalExpr->type = type;
  }
}

bool ScalarLiterals::isInsideGeneric() const {
  auto &functions = this->state.walk.functions;
  if (!functions.isInsideFunction()) {
    return false;
  }
  const FuncSymbol *outermost = functions.getOutermostDecl()->sym;

  return outermost != nullptr && outermost->isGeneric();
}

const Expr *ScalarLiterals::templatePart(const Expr *expr) const {
  auto *conversion = dyn_cast<CastExpr>(expr);
  bool isTextCast =
      conversion != nullptr && conversion->isImplicit() && conversion->type->isString();

  return isTextCast ? conversion->operand : expr;
}

bool ScalarLiterals::isConstantPart(const Expr *part) const {
  return isa<NameExpr>(part) || isa<StringLitExpr>(part) || isa<IntLitExpr>(part) ||
         isa<BoolLitExpr>(part);
}

bool ScalarLiterals::isTemplated(const Type *candidate) const {
  return candidate->isPattern() || candidate->typeOp() == TypeOp::Template ||
         candidate->isStringLiteral();
}

bool ScalarLiterals::isWantingTemplate(const Type *want) const {
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

bool ScalarLiterals::isTemplatePartType(const Type *part) const {
  return part->isString() || part->isNumeric() || part->isBool() || part->isLiteral() ||
         part->isParam();
}

std::optional<std::vector<const Type *>>
ScalarLiterals::templateOperands(const TemplateExpr *templateExpr,
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

}
