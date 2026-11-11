#include "compiler/semantic/bodies/expr/truth_checker/truth_checker.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/flow/children_first/children_first.h"
#include "compiler/semantic/flow/condition_facts/condition_facts.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/generics/generic_checker/generic_checker.h"
#include "compiler/semantic/library/json/json_checker/json_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

TruthChecker::TruthChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->callChecker = &all.callChecker;
  this->exprChecker = &all.exprChecker;
  this->jsonChecker = &all.jsonChecker;
  this->typeRelations = &all.typeRelations;
  this->conditionFacts = &all.conditionFacts;
  this->genericChecker = &all.genericChecker;
}

std::string TruthChecker::sourceText(const Expr *expr) {
  std::vector<std::string> answers;
  for (const Expr *node : ChildrenFirst::order(expr, TruthChecker::sourceParts)) {
    std::string text = TruthChecker::nodeSource(node, answers);
    if (text.empty()) {
      return "";
    }
    answers.push_back(std::move(text));
  }

  return answers.back();
}

NodeChildren<const Expr *> TruthChecker::sourceParts(const Expr *expr) {
  if (auto *conv = dyn_cast<CastExpr>(expr); conv && conv->isImplicit()) {
    return {conv->operand};
  }
  if (auto *chain = dyn_cast<OptionalChainExpr>(expr)) {
    return {chain->chain};
  }
  if (auto *member = dyn_cast<MemberExpr>(expr)) {
    return {member->object};
  }
  if (auto *index = dyn_cast<IndexExpr>(expr)) {
    return {index->object, index->index};
  }
  auto *call = dyn_cast<CallExpr>(expr);
  if (call == nullptr || call->hasSpread()) {
    return {};
  }
  NodeChildren<const Expr *> parts = {call->callee};
  parts.append(call->args.begin(), call->args.end());

  return parts;
}

std::string TruthChecker::nodeSource(const Expr *expr, std::vector<std::string> &answers) {
  if (auto *conv = dyn_cast<CastExpr>(expr); conv && conv->isImplicit()) {
    return ChildrenFirst::takeAnswer(answers);
  }
  switch (expr->kind()) {
  case Expr::Kind::Name: {
    llvm::StringRef name = cast<NameExpr>(expr)->name;
    return name.starts_with("%") ? "" : name.str();
  }
  case Expr::Kind::IntLit:
    return std::to_string(cast<IntLitExpr>(expr)->value);
  case Expr::Kind::BoolLit:
    return cast<BoolLitExpr>(expr)->value ? "true" : "false";
  case Expr::Kind::Undefined:
    return "undefined";
  case Expr::Kind::StringLit:
    return TruthChecker::stringSource(cast<StringLitExpr>(expr));
  case Expr::Kind::ArrayLit:
    return cast<ArrayLitExpr>(expr)->elements.empty() ? "[]" : "";
  case Expr::Kind::OptionalChain:
    return ChildrenFirst::takeAnswer(answers);
  case Expr::Kind::Member: {
    auto *member = cast<MemberExpr>(expr);
    std::string object = ChildrenFirst::takeAnswer(answers);
    return object + (member->optional ? "?." : ".") + member->name.str();
  }
  case Expr::Kind::Index: {
    auto *index = cast<IndexExpr>(expr);
    std::string position = ChildrenFirst::takeAnswer(answers);
    std::string object = ChildrenFirst::takeAnswer(answers);
    return object + (index->optional ? "?.[" : "[") + position + "]";
  }
  case Expr::Kind::Call:
    return TruthChecker::callSource(cast<CallExpr>(expr), answers);
  default:
    return "";
  }
}

std::string TruthChecker::stringSource(const StringLitExpr *lit) {
  std::string text = "\"";
  for (char16_t c : lit->value) {
    if (c < 0x20 || c >= 0x7f || c == '"' || c == '\\') {
      return "";
    }
    text += static_cast<char>(c);
  }

  return text + "\"";
}

std::string TruthChecker::callSource(const CallExpr *call, std::vector<std::string> &answers) {
  if (call->hasSpread()) {
    return "";
  }
  std::vector<std::string> args = ChildrenFirst::takeAnswers(answers, call->args.size());
  std::string text = ChildrenFirst::takeAnswer(answers);
  text += call->optional ? "?.(" : "(";
  for (size_t i = 0; i < args.size(); ++i) {
    text += (i > 0 ? ", " : "") + args[i];
  }

  return text + ")";
}

std::string TruthChecker::hintText(const Expr *expr, const char *placeholder) {
  std::string text = expr ? TruthChecker::sourceText(expr) : "";

  return text.empty() ? placeholder : text;
}

const NameExpr *TruthChecker::narrowedVariable(const Expr *expr) {
  auto *conv = dyn_cast<CastExpr>(expr);
  if (!conv || !conv->isImplicit() || conv->shortCircuit || !conv->operand->type->isOptional() ||
      conv->type != conv->operand->type->inner()) {
    return nullptr;
  }

  return dyn_cast<NameExpr>(conv->operand);
}

bool TruthChecker::isLength(const Expr *expr) {
  for (;;) {
    if (auto *conv = dyn_cast<CastExpr>(expr)) {
      expr = conv->operand;
    } else if (auto *chain = dyn_cast<OptionalChainExpr>(expr)) {
      expr = chain->chain;
    } else {
      break;
    }
  }
  auto *member = dyn_cast<MemberExpr>(expr);

  return member && BuiltinPropertyTable::isName(member->name, BuiltinProperty::LENGTH);
}

bool TruthChecker::isLiteral(const Expr *expr) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    expr = conv->operand;
  }
  if (auto *unary = dyn_cast<UnaryExpr>(expr); unary && unary->op == UnaryOp::Neg) {
    expr = unary->operand;
  }

  return isa<IntLitExpr>(expr) || isa<FloatLitExpr>(expr) || isa<StringLitExpr>(expr) ||
         isa<BoolLitExpr>(expr) || isa<UndefinedExpr>(expr);
}

const Type *TruthChecker::falsyType(const Type *type) {
  const Type *base = type->nonOptional();

  return base->isArray() || base->isFunction() || base->isObject() || base->isIterator() ||
                 base->isDict() || base->isClassRef()
             ? Type::undefinedTy()
             : type;
}

Expr *TruthChecker::checkCondition(Expr *cond, Flow &whenTrue, Flow &whenFalse) {
  return this->checkBranching(cond, whenTrue, whenFalse, TruthUse::Condition);
}

Expr *TruthChecker::checkBranching(Expr *expr, Flow &whenTrue, Flow &whenFalse, TruthUse use) {
  BranchRun run;
  run.steps.push_back(BranchExpr{expr, use});
  while (!run.steps.empty()) {
    BranchStep step = std::move(run.steps.back());
    run.steps.pop_back();
    this->applyBranchStep(step, run);
  }
  BranchResult result = std::move(run.results.back());
  whenTrue = std::move(result.whenTrue);
  whenFalse = std::move(result.whenFalse);

  return result.expr;
}

void TruthChecker::applyBranchStep(BranchStep &step, BranchRun &run) {
  switch (step.kind()) {
    case BranchStepKind::EXPR:
      this->enterBranch(*step.as<BranchExpr>(), run);
      break;
    case BranchStepKind::LEFT_DONE:
      this->leftDone(step.as<LeftDone>()->bin, run);
      break;
    case BranchStepKind::RIGHT_DONE:
      this->rightDone(*step.as<RightDone>(), run);
      break;
    case BranchStepKind::NOT_DONE:
      TruthChecker::notDone(step.as<NotDone>()->unary, run);
      break;
  }
}

void TruthChecker::enterBranch(const BranchExpr &branch, BranchRun &run) {
  Expr *expr = branch.expr;
  auto *bin = dyn_cast<BinaryExpr>(expr);
  if (bin && (bin->op == BinaryOp::LogAnd || bin->op == BinaryOp::LogOr)) {
    run.steps.push_back(LeftDone{bin});
    run.steps.push_back(BranchExpr{bin->lhs, TruthUse::Condition});
    return;
  }
  if (auto *unary = dyn_cast<UnaryExpr>(expr); unary && unary->op == UnaryOp::Not) {
    run.steps.push_back(NotDone{unary});
    run.steps.push_back(BranchExpr{unary->operand, TruthUse::Not});
    return;
  }
  BranchResult result;
  result.expr = this->branchLeaf(expr, result.whenTrue, result.whenFalse, branch.use);
  run.results.push_back(std::move(result));
}

void TruthChecker::leftDone(BinaryExpr *bin, BranchRun &run) {
  BranchResult left = ChildrenFirst::takeAnswer(run.results);
  bin->lhs = left.expr;
  bool isAnd = bin->op == BinaryOp::LogAnd;
  std::optional<bool> constant = this->state.getGenericStore().isCheckingInstance()
                                     ? this->genericChecker->constantCondition(bin->lhs)
                                     : std::nullopt;
  if (constant && *constant != isAnd) {
    this->state.getFlowState().replaceFlow(FlowChecker::join(left.whenTrue, left.whenFalse));
    run.results.push_back(std::move(left));
    return;
  }
  this->state.getFlowState().replaceFlow(isAnd ? left.whenTrue : left.whenFalse);
  run.steps.push_back(RightDone{bin, std::move(left.whenTrue), std::move(left.whenFalse)});
  run.steps.push_back(BranchExpr{bin->rhs, TruthUse::Condition});
}

void TruthChecker::rightDone(RightDone &done, BranchRun &run) {
  BranchResult right = ChildrenFirst::takeAnswer(run.results);
  BinaryExpr *bin = done.bin;
  bin->rhs = right.expr;
  bool isAnd = bin->op == BinaryOp::LogAnd;
  BranchResult result;
  result.expr = bin;
  result.whenTrue = isAnd ? right.whenTrue : FlowChecker::join(done.lhsTrue, right.whenTrue);
  result.whenFalse = isAnd ? FlowChecker::join(done.lhsFalse, right.whenFalse) : right.whenFalse;
  this->state.getFlowState().replaceFlow(FlowChecker::join(result.whenTrue, result.whenFalse));
  this->jsonChecker->walkRedundantTests(bin);
  bin->type = Type::boolTy();
  run.results.push_back(std::move(result));
}

void TruthChecker::notDone(UnaryExpr *unary, BranchRun &run) {
  BranchResult operand = ChildrenFirst::takeAnswer(run.results);
  unary->operand = operand.expr;
  unary->type = Type::boolTy();
  BranchResult result;
  result.expr = unary;
  result.whenTrue = std::move(operand.whenFalse);
  result.whenFalse = std::move(operand.whenTrue);
  run.results.push_back(std::move(result));
}

Expr *TruthChecker::branchLeaf(Expr *expr, Flow &whenTrue, Flow &whenFalse, TruthUse use) {
  if (this->isBooleanCall(expr)) {
    return this->checkBooleanCall(cast<CallExpr>(expr), whenTrue, whenFalse);
  }
  if (auto *assign = dyn_cast<AssignExpr>(expr); assign && !assign->op) {
    expr = this->exprChecker->checkExpr(assign);
    whenTrue = this->state.getFlowState().getFlow();
    whenFalse = this->state.getFlowState().getFlow();
    ResultConditionForm form = TruthChecker::assignmentCondition(assign, use);
    if (form.isErr()) {
      this->state.getDiagnostics().error(assign->loc(), form.error().message);
    }
    return this->typeRelations->convert(expr, Type::boolTy());
  }

  expr = this->exprChecker->checkExpr(expr, Type::boolTy());
  whenTrue = this->state.getFlowState().getFlow();
  whenFalse = this->state.getFlowState().getFlow();
  if (auto *lit = dyn_cast<BoolLitExpr>(expr)) {
    (lit->value ? whenFalse : whenTrue).unreachable = true;
  } else {
    this->conditionFacts->addConditionFacts(expr, whenTrue, whenFalse);
  }
  if (!expr->type->isBool()) {
    ResultTruthIsTested truthy = this->checkTruthy(expr, use);
    if (truthy.isErr()) {
      this->state.getDiagnostics().error(expr->loc(), truthy.error().message);
    }
  }

  return this->typeRelations->convert(expr, Type::boolTy());
}

Expr *TruthChecker::checkTestedValue(Expr *expr, const Type *expected, Flow &whenTrue,
                                     Flow &whenFalse, TruthUse use) {
  if (auto *bin = dyn_cast<BinaryExpr>(expr);
      bin && (bin->op == BinaryOp::LogAnd || bin->op == BinaryOp::LogOr)) {
    return this->checkLogical(bin, expected, whenTrue, whenFalse, true,
                              bin->op == BinaryOp::LogAnd ? TruthUse::And : TruthUse::Or);
  }
  if (auto *unary = dyn_cast<UnaryExpr>(expr);
      (unary && unary->op == UnaryOp::Not) || this->isBooleanCall(expr)) {
    return this->checkBranching(expr, whenTrue, whenFalse, use);
  }
  expr = this->exprChecker->checkExpr(expr, expected);
  whenTrue = this->state.getFlowState().getFlow();
  whenFalse = this->state.getFlowState().getFlow();
  if (auto *lit = dyn_cast<BoolLitExpr>(expr)) {
    (lit->value ? whenFalse : whenTrue).unreachable = true;
  } else {
    this->conditionFacts->addConditionFacts(expr, whenTrue, whenFalse);
  }

  return expr;
}

Expr *TruthChecker::checkLogical(BinaryExpr *bin, const Type *expected, Flow &whenTrue,
                                 Flow &whenFalse, bool tested, TruthUse use) {
  bool isAnd = bin->op == BinaryOp::LogAnd;
  bool isAssign = use == TruthUse::OrAssign || use == TruthUse::AndAssign;
  const Expr *other = isAssign ? cast<AssignExpr>(bin->rhs)->value : bin->rhs;
  Flow lhsTrue;
  Flow lhsFalse;
  Flow rhsTrue;
  Flow rhsFalse;
  Expr *left = this->checkTestedValue(bin->lhs, expected, lhsTrue, lhsFalse, use);
  bin->lhs = this->genericChecker->apparent(left);
  ResultTruthIsTested leftTruthy = this->checkTruthy(bin->lhs, use, other);
  if (leftTruthy.isErr()) {
    this->state.getDiagnostics().error(bin->lhs->loc(), leftTruthy.error().message);
  }
  bool isTruthy = leftTruthy.isOk();
  const Type *l = bin->lhs->type;
  this->state.getFlowState().replaceFlow(isAnd ? lhsTrue : lhsFalse);
  const Type *hint = isAnd || l->isError() || l->isUndefined() ? expected : l->nonOptional();
  if (tested) {
    bin->rhs = this->checkTestedValue(bin->rhs, hint, rhsTrue, rhsFalse, TruthUse::Condition);
    ResultTruthIsTested rightTruthy = this->checkTruthy(bin->rhs, TruthUse::Condition);
    if (rightTruthy.isErr()) {
      this->state.getDiagnostics().error(bin->rhs->loc(), rightTruthy.error().message);
    }
    isTruthy &= rightTruthy.isOk();
  } else {
    bin->rhs = this->exprChecker->checkExpr(bin->rhs, hint);
    rhsTrue = this->state.getFlowState().getFlow();
    rhsFalse = this->state.getFlowState().getFlow();
  }
  whenTrue = isAnd ? rhsTrue : FlowChecker::join(lhsTrue, rhsTrue);
  whenFalse = isAnd ? FlowChecker::join(lhsFalse, rhsFalse) : rhsFalse;
  this->state.getFlowState().replaceFlow(FlowChecker::join(whenTrue, whenFalse));
  if (!isAssign) {
    this->jsonChecker->walkRedundantTests(bin);
  }

  return this->checkLogicalType(bin, l, isTruthy, use);
}

Expr *TruthChecker::checkLogicalType(BinaryExpr *bin, const Type *l, bool truthy, TruthUse use) {
  bool isAnd = bin->op == BinaryOp::LogAnd;
  bool isAssign = use == TruthUse::OrAssign || use == TruthUse::AndAssign;
  const char *op = isAnd ? (isAssign ? "&&=" : "&&") : (isAssign ? "||=" : "||");
  const Type *r = bin->rhs->type;
  bin->type = Type::error();
  if (!truthy) {
    return bin;
  }
  const Type *kept = l->isError() ? l : isAnd ? TruthChecker::falsyType(l) : l->nonOptional();
  if (l->isError() || r->isError()) {
    if (this->state.getFunctionStack().isInsideTrial() && l->isError() != r->isError()) {
      bin->type = l->isError() ? r : kept;
    }
    return bin;
  }
  if (l->isBool() && r->isBool()) {
    bin->type = l;
    return bin;
  }
  ResultLogicalJoined joined = TruthChecker::logicalValue(bin, op, isAnd, kept);
  if (joined.isErr()) {
    this->state.getDiagnostics().error(bin->loc(), joined.error().message);
    return bin;
  }
  const Type *result = joined.value();
  bin->rhs = this->typeRelations->convert(bin->rhs, result);
  bin->type = result;

  return bin;
}

llvm::StringRef TruthChecker::useOperator(TruthUse use) {
  switch (use) {
    case TruthUse::Condition:
      return "";
    case TruthUse::Not:
      return "!";
    case TruthUse::Or:
      return "||";
    case TruthUse::And:
      return "&&";
    case TruthUse::OrAssign:
      return "||=";
    case TruthUse::AndAssign:
      return "&&=";
  }

  return "";
}

std::string TruthChecker::paramMessage(const Type *type, const Type *base, TruthUse use,
                                       llvm::StringRef x, llvm::StringRef y) {
  bool isNegated = use == TruthUse::Not;
  std::string what = ErrorTextTruthy::typeValue(type->name());
  std::string head = ErrorTextTruthy::cannotTest(TruthChecker::useOperator(use), what) +
                     ErrorTextTruthy::mayBeNum(base->name());
  if (use == TruthUse::Or) {
    return head + ErrorTextTruthy::orDefault(x, y);
  }
  if (use == TruthUse::OrAssign) {
    return head + ErrorTextTruthy::orAssign(x, y);
  }
  if (use == TruthUse::AndAssign) {
    return head + ErrorTextTruthy::andAssign(x, y);
  }

  return head + ErrorTextTruthy::undefTest(x, isNegated);
}

std::string TruthChecker::jsonMessage(TruthUse use, llvm::StringRef x, llvm::StringRef y) {
  bool isNegated = use == TruthUse::Not;
  std::string head =
      ErrorTextTruthy::jsonHead(TruthChecker::useOperator(use)) + ErrorTextTruthy::jsonMay();
  if (use == TruthUse::Or) {
    return head + ErrorTextTruthy::jsonOr(x, y);
  }
  if (use == TruthUse::OrAssign) {
    return head + ErrorTextTruthy::orAssign(x, y);
  }

  return head + ErrorTextTruthy::jsonTest(x, isNegated);
}

std::string TruthChecker::unknownMessage(TruthUse use, llvm::StringRef x, llvm::StringRef y) {
  bool isNegated = use == TruthUse::Not;
  std::string head =
      ErrorTextTruthy::unknownHd(TruthChecker::useOperator(use)) + ErrorTextTruthy::unknownMay();
  if (use == TruthUse::Or) {
    return head + ErrorTextTruthy::orDefault(x, y);
  }
  if (use == TruthUse::OrAssign) {
    return head + ErrorTextTruthy::orAssign(x, y);
  }

  return head + ErrorTextTruthy::unknownTst(x, isNegated);
}

std::string TruthChecker::numberMessage(const Expr *expr, const Type *type, const Type *base,
                                        TruthUse use, llvm::StringRef x, llvm::StringRef y) {
  bool isNegated = use == TruthUse::Not;
  bool isLength = TruthChecker::isLength(expr);
  std::string what = ErrorTextTruthy::number(base->isDecimal(), type->isOptional());
  std::string head =
      ErrorTextTruthy::cannotTest(TruthChecker::useOperator(use), what) + ErrorTextTruthy::zero();
  if (type->isOptional()) {
    head += ErrorTextTruthy::too();
    bool isLengthUse =
        isLength && (use == TruthUse::Condition || use == TruthUse::Not || use == TruthUse::And);
    if (isLengthUse) {
      return head + ErrorTextTruthy::lengthTest(x, isNegated);
    }
    if (use == TruthUse::Or) {
      return head + ErrorTextTruthy::orDefault(x, y);
    }
    if (use == TruthUse::OrAssign) {
      return head + ErrorTextTruthy::orAssign(x, y);
    }
    return head + ErrorTextTruthy::undefTest(x, isNegated);
  }
  if (base->isFloat()) {
    return head + ErrorTextTruthy::floatTest(x, isNegated);
  }

  std::string test = ErrorTextTruthy::zeroTest(x, isNegated, isLength);
  if (use == TruthUse::Or) {
    return head + ErrorTextTruthy::orCompare(test, x, y);
  }
  if (use == TruthUse::OrAssign) {
    return head + ErrorTextTruthy::orAssignIf(x, y);
  }
  if (use == TruthUse::AndAssign) {
    return head + ErrorTextTruthy::andIf(test, x, y);
  }

  return head + ErrorTextTruthy::compare(test);
}

std::string TruthChecker::objectMessage(const Expr *expr, const Type *base, TruthUse use,
                                        llvm::StringRef x, llvm::StringRef y) {
  ObjectTruth truth = TruthChecker::objectTruth(expr, base);
  std::string message = TruthChecker::objectHead(truth, use);
  std::string advice = TruthChecker::objectAdvice(truth, base, use, x, y);

  return message + advice;
}

TruthChecker::ObjectTruth TruthChecker::objectTruth(const Expr *expr, const Type *base) {
  ObjectTruth truth;
  truth.narrowed = TruthChecker::narrowedVariable(expr);
  truth.isArray = base->isArray();
  truth.isPromise = base->isPromise();
  truth.isClass = base->isClassRef();
  truth.isObject =
      base->isObject() || base->isIterator() || base->isDict() || truth.isClass || truth.isPromise;
  const Expr *bare = expr;
  while (isa<CastExpr>(bare) && cast<CastExpr>(bare)->isImplicit()) {
    bare = cast<CastExpr>(bare)->operand;
  }
  auto *name = dyn_cast<NameExpr>(bare);
  truth.className =
      truth.isClass && name != nullptr && name->cls() != nullptr && name->var() == nullptr;

  return truth;
}

std::string TruthChecker::objectHead(const ObjectTruth &truth, TruthUse use) {
  std::string what =
      ErrorTextTruthy::kind(truth.isArray, truth.isPromise, truth.isClass, truth.isObject);
  std::string message;
  if (truth.narrowed != nullptr) {
    message = ErrorTextTruthy::holds(truth.narrowed->name, what);
  } else if (truth.isArray) {
    message = ErrorTextTruthy::arrayTrue();
  } else {
    message = ErrorTextTruthy::alwaysTrue(what);
  }
  std::string consequence = TruthChecker::useConsequence(use);

  return message + consequence;
}

std::string TruthChecker::useConsequence(TruthUse use) {
  switch (use) {
    case TruthUse::Condition:
      return ErrorTextTruthy::soAlways();
    case TruthUse::Not:
      return ErrorTextTruthy::soNot();
    case TruthUse::Or:
      return ErrorTextTruthy::soOr();
    case TruthUse::And:
      return ErrorTextTruthy::soAnd();
    case TruthUse::OrAssign:
      return ErrorTextTruthy::soOrSet();
    case TruthUse::AndAssign:
      return ErrorTextTruthy::soAndSet();
  }

  return "";
}

std::string TruthChecker::objectAdvice(const ObjectTruth &truth, const Type *base, TruthUse use,
                                       llvm::StringRef x, llvm::StringRef y) {
  bool isNegated = use == TruthUse::Not;
  std::string length = ErrorTextTruthy::lengthCmp(x, isNegated);
  std::string call =
      truth.isObject ? "" : ErrorTextTruthy::callOf(x, isNegated, base->minArgs() == 0);
  if (use == TruthUse::AndAssign) {
    return ErrorTextTruthy::useAssign();
  }
  if (use == TruthUse::Or) {
    return ErrorTextTruthy::dropOr(y);
  }
  if (use == TruthUse::OrAssign) {
    return truth.isArray ? ErrorTextTruthy::arrayLen(x, y) : ErrorTextTruthy::dropAssign();
  }
  if ((truth.narrowed != nullptr || truth.isObject) && use == TruthUse::And) {
    return ErrorTextTruthy::dropAnd(x);
  }
  if (truth.narrowed != nullptr || truth.className) {
    return truth.isArray ? ErrorTextTruthy::dropOrLen(length) : ErrorTextTruthy::dropCheck();
  }
  if (truth.isArray) {
    return ErrorTextTruthy::checkLen(length);
  }
  if (truth.isObject) {
    return ErrorTextTruthy::allowUndef(base->name());
  }

  return ErrorTextTruthy::callIt(call);
}

TruthChecker::ResultTruthIsTested TruthChecker::checkTruthy(const Expr *expr, TruthUse use,
                                                            const Expr *other) {
  const Type *type = expr->type;
  const Type *base = GenericChecker::apparentType(type->nonOptional());
  std::optional<TruthCheckerCheckTruthyErrorCode> problem = TruthChecker::truthProblem(type, base);
  if (!problem.has_value()) {
    return ok(expr);
  }
  std::string message = TruthChecker::truthMessage(*problem, expr, use, other, base);

  return err(TruthCheckerCheckTruthyError{
      .code = *problem,
      .message = message,
  });
}

std::optional<TruthCheckerCheckTruthyErrorCode> TruthChecker::truthProblem(const Type *type,
                                                                           const Type *base) {
  if (TruthChecker::isTestable(type, base)) {
    return std::nullopt;
  }
  if (base->isUnion() || base->isLiteral()) {
    return TruthChecker::unionProblem(type, base);
  }
  if (type->isVoid()) {
    return TruthCheckerCheckTruthyErrorCode::VOID_VALUE;
  }
  if (type->isUndefined()) {
    return TruthCheckerCheckTruthyErrorCode::ALWAYS_UNDEFINED;
  }
  if (base->isParam()) {
    return TruthCheckerCheckTruthyErrorCode::PARAM_MAY_BE_NUMBER;
  }
  if (base->isJson()) {
    return TruthCheckerCheckTruthyErrorCode::JSON_VALUE;
  }
  if (base->isUnknown()) {
    return TruthCheckerCheckTruthyErrorCode::UNKNOWN_VALUE;
  }
  if (base->isResult()) {
    return TruthCheckerCheckTruthyErrorCode::RESULT_VALUE;
  }
  if (base->isNumeric() || base->isDecimal()) {
    return TruthCheckerCheckTruthyErrorCode::NUMBER_VALUE;
  }

  return TruthCheckerCheckTruthyErrorCode::ALWAYS_TRUTHY;
}

bool TruthChecker::isTestable(const Type *type, const Type *base) {
  if (type->isUndefined() || base->isNever()) {
    return true;
  }
  bool isOptionalObject =
      type->isOptional() &&
      (base->isArray() || base->isFunction() || base->isObject() || base->isResult() ||
       base->isIterator() || base->isDict() || base->isPromise() || base->isClassRef());
  if (type->isError() || base->isBool() || base->isString() || isOptionalObject) {
    return true;
  }

  return base->isParam() && !base->alternatives().empty() &&
         TruthChecker::allBoolOrString(base->alternatives());
}

bool TruthChecker::allBoolOrString(llvm::ArrayRef<const Type *> types) {
  for (const Type *t : types) {
    if (!t->isBool() && !t->isString()) {
      return false;
    }
  }

  return true;
}

std::optional<TruthCheckerCheckTruthyErrorCode> TruthChecker::unionProblem(const Type *type,
                                                                           const Type *base) {
  const Type *number = TruthChecker::unionNumber(base);
  bool isFalsy = type->isOptional() || TruthChecker::hasFalsyMember(base);
  if (number == nullptr && isFalsy) {
    return std::nullopt;
  }
  if (number == nullptr) {
    return TruthCheckerCheckTruthyErrorCode::UNION_ALWAYS_TRUTHY;
  }

  return TruthCheckerCheckTruthyErrorCode::UNION_WITH_NUMBER;
}

const Type *TruthChecker::unionNumber(const Type *base) {
  llvm::ArrayRef<const Type *> members =
      base->isUnion() ? base->members() : llvm::ArrayRef<const Type *>(base);
  const Type *number = nullptr;
  for (const Type *m : members) {
    const Type *b = m->isLiteral() ? m->literalBase() : m;
    if (b->isNumeric() || b->isDecimal() || b->isParam() || b->isJson()) {
      number = m;
    }
  }

  return number;
}

bool TruthChecker::hasFalsyMember(const Type *base) {
  llvm::ArrayRef<const Type *> members =
      base->isUnion() ? base->members() : llvm::ArrayRef<const Type *>(base);
  for (const Type *m : members) {
    const Type *b = m->isLiteral() ? m->literalBase() : m;
    bool isEmpty = m->isStringLiteral() ? m->literalText().empty() : m->literalNumber() == 0;
    if (m->isLiteral() ? isEmpty : b->isBool() || b->isString()) {
      return true;
    }
  }

  return false;
}

std::string TruthChecker::truthMessage(TruthCheckerCheckTruthyErrorCode code, const Expr *expr,
                                       TruthUse use, const Expr *other, const Type *base) {
  const Type *type = expr->type;
  bool isNegated = use == TruthUse::Not;
  std::string x = TruthChecker::hintText(expr);
  std::string y = TruthChecker::hintText(other, "...");
  switch (code) {
    case TruthCheckerCheckTruthyErrorCode::UNION_ALWAYS_TRUTHY:
      return ErrorTextTruthy::unionTrue(type->name());
    case TruthCheckerCheckTruthyErrorCode::UNION_WITH_NUMBER:
      return TruthChecker::unionNumberMessage(type, base, x);
    case TruthCheckerCheckTruthyErrorCode::VOID_VALUE:
      return ErrorTextTruthy::voidValue();
    case TruthCheckerCheckTruthyErrorCode::ALWAYS_UNDEFINED:
      return ErrorTextTruthy::undefined();
    case TruthCheckerCheckTruthyErrorCode::PARAM_MAY_BE_NUMBER:
      return TruthChecker::paramMessage(type, base, use, x, y);
    case TruthCheckerCheckTruthyErrorCode::JSON_VALUE:
      return TruthChecker::jsonMessage(use, x, y);
    case TruthCheckerCheckTruthyErrorCode::UNKNOWN_VALUE:
      return TruthChecker::unknownMessage(use, x, y);
    case TruthCheckerCheckTruthyErrorCode::RESULT_VALUE:
      return ErrorTextTruthy::result(x, isNegated);
    case TruthCheckerCheckTruthyErrorCode::NUMBER_VALUE:
      return TruthChecker::numberMessage(expr, type, base, use, x, y);
    case TruthCheckerCheckTruthyErrorCode::ALWAYS_TRUTHY:
      return TruthChecker::objectMessage(expr, base, use, x, y);
  }

  return TruthChecker::objectMessage(expr, base, use, x, y);
}

std::string TruthChecker::unionNumberMessage(const Type *type, const Type *base,
                                             llvm::StringRef value) {
  const Type *number = TruthChecker::unionNumber(base);

  return ErrorTextTruthy::unionNum(type->name(), number->name(), value);
}

TruthChecker::ResultConditionForm TruthChecker::assignmentCondition(const AssignExpr *assign,
                                                                    TruthUse use) {
  bool isNegated = use == TruthUse::Not;
  std::string target = TruthChecker::hintText(assign->target);
  std::string value = TruthChecker::hintText(assign->value, "...");
  const Type *type = assign->type;
  bool isCompareInstead =
      TruthChecker::isLiteral(assign->value) || type->isError() ||
      !(type->isOptional() || type->isNumeric() || type->isDecimal() || type->isString());
  if (isCompareInstead) {
    std::string eq = isNegated ? "!=" : "==";
    std::string message = ErrorTextAssignTest::compare(eq, target, value);

    return err(TruthCheckerAssignmentConditionError{
        .code = TruthCheckerAssignmentConditionErrorCode::COMPARE_INSTEAD,
        .message = message,
    });
  }

  std::string message = ErrorTextAssignTest::testValue(target, value, isNegated, type->isOptional(),
                                                       type->isString());

  return err(TruthCheckerAssignmentConditionError{
      .code = TruthCheckerAssignmentConditionErrorCode::TEST_ASSIGNED,
      .message = message,
  });
}

TruthChecker::ResultLogicalJoined TruthChecker::logicalValue(const BinaryExpr *bin,
                                                             llvm::StringRef op, bool isAnd,
                                                             const Type *kept) {
  const Type *r = bin->rhs->type;
  if (r->isVoid()) {
    std::string test = TruthChecker::hintText(bin->lhs);
    std::string rhs = TruthChecker::hintText(bin->rhs, "...");
    std::string message = ErrorTextLogical::rightVoid(op, isAnd, test, rhs);

    return err(TruthCheckerLogicalValueError{
        .code = TruthCheckerLogicalValueErrorCode::RIGHT_SIDE_VOID,
        .message = message,
    });
  }

  const Type *result = TypeRelations::unify(kept, r);
  if (!result) {
    std::string message = ErrorTextLogical::noCommon(op, kept->name(), isAnd, r->name());

    return err(TruthCheckerLogicalValueError{
        .code = TruthCheckerLogicalValueErrorCode::NO_COMMON_TYPE,
        .message = message,
    });
  }

  return ok(result);
}

bool TruthChecker::isBooleanCall(const Expr *expr) const {
  auto *call = dyn_cast<CallExpr>(expr);

  return call && !call->optional && call->args.size() == 1 && !call->hasSpread() &&
         this->callChecker->isGlobalName(call->callee, GlobalName::BOOLEAN);
}

Expr *TruthChecker::checkBooleanCall(CallExpr *call, Flow &whenTrue, Flow &whenFalse) {
  return this->checkBranching(call->args[0], whenTrue, whenFalse);
}

}
