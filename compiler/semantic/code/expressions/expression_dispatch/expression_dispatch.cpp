#include "compiler/semantic/code/expressions/expression_dispatch/expression_dispatch.h"
#include "compiler/program/library/builtin_names/keyword_table/keyword_table.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

ExpressionDispatch::ExpressionDispatch(SemanticState &owner)
    : state(owner) {}

void ExpressionDispatch::checkExpr(const CheckExpr &task) {
  const auto *forced = this->findForcedType(task.expr);
  if (forced != nullptr) {
    this->state.walk.tasks.pushTask(ForceConversion{.to = forced});
  }
  auto *generic = this->findGenericFunction(task.expr);
  if (generic != nullptr) {
    auto wanted = this->expectedSignature(task.expected);

    return this->state.walk.tasks.pushTask(CheckGenericValue{
        .name = cast<NameExpr>(task.expr),
        .generic = generic,
        .wanted = wanted,
    });
  }
  bool isNamed = isa<NameExpr>(task.expr) || isa<MemberExpr>(task.expr);
  std::optional<Signature> signature =
      isNamed ? this->expectedSignature(task.expected) : std::nullopt;
  if (signature.has_value()) {
    return this->state.walk.tasks.pushTask(CheckFunctionValue{
        .expr = task.expr,
        .expected = task.expected,
        .signature = *signature,
    });
  }
  auto kind = CheckExprKind{.expr = task.expr, .expected = task.expected};

  return this->checkExprKind(kind);
}

void ExpressionDispatch::checkExprSlot(const CheckExprSlot &task) {
  this->state.walk.tasks.pushTask(WriteCheckedSlot{.slot = task.slot});

  return this->state.walk.tasks.pushTask(CheckExpr{.expr = *task.slot, .expected = task.expected});
}

void ExpressionDispatch::writeCheckedSlot(const WriteCheckedSlot &task) {
  *task.slot = this->state.walk.tasks.takeChecked();
}

void ExpressionDispatch::reportError(const ReportError &task) {
  return this->state.diagnostics.error(task.loc, task.message);
}

const Type *ExpressionDispatch::findForcedType(const Expr *expr) const {
  const auto &copies = this->state.generics.copies;
  bool isForcing = copies.isCheckingInstance() && copies.hasForcedTypes();

  return isForcing ? copies.findForcedType(expr) : nullptr;
}

FuncSymbol *ExpressionDispatch::findGenericFunction(const Expr *expr) const {
  const auto *name = dyn_cast<NameExpr>(expr);
  if (name == nullptr) {
    return nullptr;
  }
  auto entry = this->state.walk.scopes.findName(name->name);
  FuncSymbol *function = entry.func();
  if (function == nullptr || entry.var() != nullptr || !function->isGeneric()) {
    return nullptr;
  }

  return function;
}

void ExpressionDispatch::checkExprKind(const CheckExprKind &task) {
  Expr *expr = task.expr;
  const Type *expected = task.expected;
  auto &tasks = this->state.walk.tasks;
  switch (expr->kind()) {
    case Expr::Kind::IntLit:
      return tasks.pushTask(CheckIntLiteral{
          .literal = cast<IntLitExpr>(expr),
          .expected = expected,
          .sign = LiteralSign::PLAIN,
      });
    case Expr::Kind::FloatLit:
      return this->checkFloatLiteral(cast<FloatLitExpr>(expr), expected);
    case Expr::Kind::BoolLit:
      return this->checkBoolLiteral(cast<BoolLitExpr>(expr), expected);
    case Expr::Kind::StringLit:
      return this->checkStringLiteral(cast<StringLitExpr>(expr), expected);
    case Expr::Kind::RegexLit:
      return tasks.pushTask(CheckRegexLiteral{.literal = cast<RegexLitExpr>(expr)});
    case Expr::Kind::Undefined:
      expr->type = Type::undefinedTy();
      return tasks.pushChecked(expr);
    case Expr::Kind::Template:
      return tasks.pushTask(
          CheckTemplate{.templateExpr = cast<TemplateExpr>(expr), .expected = expected});
    case Expr::Kind::ArrayLit:
      return tasks.pushTask(
          CheckArrayLiteral{.literal = cast<ArrayLitExpr>(expr), .expected = expected});
    case Expr::Kind::ObjectLit:
      return tasks.pushTask(
          CheckObjectLiteral{.literal = cast<ObjectLitExpr>(expr), .expected = expected});
    case Expr::Kind::Name:
      return this->checkNameValue(cast<NameExpr>(expr), expected);
    case Expr::Kind::Member:
      return tasks.pushTask(CheckMember{.member = cast<MemberExpr>(expr), .expected = expected});
    case Expr::Kind::Index:
      return tasks.pushTask(CheckIndex{.index = cast<IndexExpr>(expr)});
    case Expr::Kind::Unary:
      return tasks.pushTask(CheckUnary{.unary = cast<UnaryExpr>(expr), .expected = expected});
    case Expr::Kind::Binary:
      return tasks.pushTask(CheckBinary{.binary = cast<BinaryExpr>(expr), .expected = expected});
    case Expr::Kind::Assign:
      return tasks.pushTask(CheckAssign{.assign = cast<AssignExpr>(expr)});
    case Expr::Kind::Update:
      return tasks.pushTask(CheckUpdate{.update = cast<UpdateExpr>(expr)});
    case Expr::Kind::Call:
      return tasks.pushTask(CheckCall{.call = cast<CallExpr>(expr), .expected = expected});
    case Expr::Kind::New:
      return tasks.pushTask(CheckNew{.newExpr = cast<NewExpr>(expr), .expected = expected});
    case Expr::Kind::Class:
      return tasks.pushTask(CheckClassExpr{.classExpr = cast<ClassExpr>(expr)});
    case Expr::Kind::Conditional:
      return this->startConditional(cast<ConditionalExpr>(expr), expected);
    case Expr::Kind::Cast:
      return this->startCast(cast<CastExpr>(expr));
    case Expr::Kind::Sequence:
      return tasks.pushTask(NextSequenceItem{
          .sequence = cast<SequenceExpr>(expr),
          .index = 0,
          .expected = expected,
      });
    case Expr::Kind::OptionalChain:
      return this->startOptionalChain(cast<OptionalChainExpr>(expr));
    case Expr::Kind::Function:
      return this->checkFunctionLiteral(cast<FunctionExpr>(expr), expected);
    case Expr::Kind::Yield:
      return tasks.pushTask(CheckYield{.yield = cast<YieldExpr>(expr)});
    case Expr::Kind::Await:
      return tasks.pushTask(CheckAwait{.await = cast<AwaitExpr>(expr), .expected = expected});
  }

  return tasks.pushChecked(expr);
}

std::optional<Signature> ExpressionDispatch::expectedSignature(const Type *expected) const {
  const Type *function = expected != nullptr ? expected->nonOptional() : nullptr;
  if (function == nullptr || !function->isFunction()) {
    return std::nullopt;
  }

  return this->signatureOf(function);
}

Signature ExpressionDispatch::signatureOf(const Type *functionType) const {
  llvm::ArrayRef<const Type *> params = functionType->params();

  return Signature{
      .params = std::vector<const Type *>(params.begin(), params.end()),
      .minArgs = functionType->minArgs(),
      .ret = functionType->ret(),
      .voidResult = functionType->ret()->isVoid(),
      .hasThis = functionType->hasThis(),
  };
}

void ExpressionDispatch::checkFloatLiteral(FloatLitExpr *literalExpr, const Type *expected) {
  const auto *literal = this->state.types.literals.numberLiteralFor(expected, literalExpr->value,
                                                                    NumberSpelling::FRACTION);
  if (literal != nullptr) {
    literalExpr->type = literal;
    return this->state.walk.tasks.pushChecked(literalExpr);
  }
  const auto *member =
      this->state.types.literals.numberMemberOf(expected, NumberSpelling::FRACTION);
  bool isDecimal = expected != nullptr && (expected->nonOptional()->isDecimal() ||
                                           (member != nullptr && member->isDecimal()));
  if (isDecimal) {
    literalExpr->type = Type::decimalTy();
    this->state.walk.tasks.pushTask(CheckDecimalLiteralRange{.literal = literalExpr});
    return this->state.walk.tasks.pushChecked(literalExpr);
  }
  literalExpr->type = Type::doubleTy();

  return this->state.walk.tasks.pushChecked(literalExpr);
}

void ExpressionDispatch::checkBoolLiteral(BoolLitExpr *literalExpr, const Type *expected) {
  const auto *literal = this->state.types.literals.boolLiteralFor(expected, literalExpr->value);
  literalExpr->type = literal != nullptr ? literal : Type::boolTy();

  return this->state.walk.tasks.pushChecked(literalExpr);
}

void ExpressionDispatch::checkStringLiteral(StringLitExpr *literalExpr, const Type *expected) {
  const auto *literal = this->state.types.literals.stringLiteralFor(expected, literalExpr->value);
  literalExpr->type = literal != nullptr ? literal : Type::string();

  return this->state.walk.tasks.pushChecked(literalExpr);
}

void ExpressionDispatch::checkNameValue(NameExpr *name, const Type *expected) {
  if (this->isJsonNull(name, expected)) {
    name->type = Type::json();
    return this->state.walk.tasks.pushChecked(name);
  }
  auto *enumDecl = this->findEnumObject(name, expected);
  if (enumDecl != nullptr) {
    return this->state.walk.tasks.pushTask(CheckEnumObject{
        .name = name,
        .enumDecl = enumDecl,
        .expected = expected,
    });
  }
  if (expected != nullptr) {
    this->state.walk.tasks.pushTask(ConstText{.expected = expected});
  }

  return this->state.walk.tasks.pushTask(CheckName{.name = name});
}

bool ExpressionDispatch::isJsonNull(const NameExpr *name, const Type *expected) const {
  bool isNullName = KeywordTable::find(name->name) == Keyword::NULL_KEYWORD &&
                    name->var() == nullptr && name->func() == nullptr;

  return isNullName && expected != nullptr && expected->nonOptional()->isJson();
}

EnumDecl *ExpressionDispatch::findEnumObject(const NameExpr *name, const Type *expected) const {
  const Type *target = expected != nullptr ? expected->nonOptional() : nullptr;
  if (target == nullptr || !target->isDict()) {
    return nullptr;
  }

  return this->state.walk.scopes.findName(name->name).enumDecl();
}

void ExpressionDispatch::checkFunctionLiteral(FunctionExpr *functionExpr, const Type *expected) {
  auto signature = this->expectedSignature(expected);

  return this->state.walk.tasks.pushTask(CheckFunctionExpr{
      .functionExpr = functionExpr,
      .expected = signature,
  });
}

void ExpressionDispatch::nextSequenceItem(const NextSequenceItem &task) {
  SequenceExpr *sequence = task.sequence;
  size_t last = sequence->exprs.size() - 1;
  if (task.index == last) {
    this->state.walk.tasks.pushTask(FinishSequence{.sequence = sequence});
    return this->state.walk.tasks.pushTask(
        CheckExpr{.expr = sequence->exprs[last], .expected = task.expected});
  }
  this->state.walk.tasks.pushTask(AfterSequenceItem{
      .sequence = sequence,
      .index = task.index,
      .expected = task.expected,
  });

  return this->state.walk.tasks.pushTask(
      CheckExpr{.expr = sequence->exprs[task.index], .expected = nullptr});
}

void ExpressionDispatch::afterSequenceItem(const AfterSequenceItem &task) {
  Expr *item = this->state.walk.tasks.takeChecked();
  task.sequence->exprs[task.index] = item;
  this->state.walk.tasks.pushTask(NextSequenceItem{
      .sequence = task.sequence,
      .index = task.index + 1,
      .expected = task.expected,
  });

  return this->state.walk.tasks.pushTask(CheckUsedValue{.expr = item});
}

void ExpressionDispatch::finishSequence(const FinishSequence &task) {
  Expr *last = this->state.walk.tasks.takeChecked();
  task.sequence->exprs.back() = last;
  task.sequence->type = last->type;

  return this->state.walk.tasks.pushChecked(task.sequence);
}

void ExpressionDispatch::startOptionalChain(OptionalChainExpr *chain) {
  this->state.walk.functions.enterOptionalChain();
  auto before = this->state.walk.flow.getFlow();
  this->state.walk.tasks.pushTask(FinishOptionalChain{.chain = chain, .before = std::move(before)});

  return this->state.walk.tasks.pushTask(CheckExpr{.expr = chain->chain, .expected = nullptr});
}

void ExpressionDispatch::finishOptionalChain(const FinishOptionalChain &task) {
  OptionalChainExpr *chain = task.chain;
  chain->chain = this->state.walk.tasks.takeChecked();
  chain->mayShortCircuit = this->state.walk.functions.isShortCircuiting();
  this->state.walk.functions.leaveOptionalChain();
  const Type *type = chain->chain->type;
  chain->type = type;
  this->state.walk.tasks.pushChecked(chain);
  if (!chain->mayShortCircuit || type->isError()) {
    return;
  }
  if (!type->isVoid()) {
    chain->type = Type::optional(type);
  }
  const auto &after = this->state.walk.flow.getFlow();
  auto joined = this->state.flow.joins.join(task.before, after);

  return this->state.walk.flow.replaceFlow(std::move(joined));
}

void ExpressionDispatch::startConditional(ConditionalExpr *conditional, const Type *expected) {
  this->state.walk.tasks.pushTask(
      AfterConditionTest{.conditional = conditional, .expected = expected});

  return this->state.walk.tasks.pushTask(CheckCondition{.cond = conditional->cond});
}

void ExpressionDispatch::startCast(CastExpr *castExpr) {
  this->state.walk.tasks.pushTask(AfterCastTarget{.castExpr = castExpr});

  return this->state.walk.tasks.pushTask(
      ResolveTypeRef{.ref = &*castExpr->target, .isVoidAllowed = false});
}

}
