#include "compiler/semantic/bodies/expr/truth_checker/truth_checker.h"
#include "compiler/semantic/bodies/expr/call_checker/call_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/flow/condition_facts/condition_facts.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/generics/generic_checker/generic_checker.h"
#include "compiler/semantic/library/json/json_checker/json_checker.h"
#include "compiler/semantic/types/type_relations/type_relations.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

std::string TruthChecker::sourceText(const Expr *expr) {
  if (auto *conv = dyn_cast<CastExpr>(expr); conv && conv->isImplicit()) {
    return TruthChecker::sourceText(conv->operand);
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
  case Expr::Kind::StringLit: {
    std::string text = "\"";
    for (char16_t c : cast<StringLitExpr>(expr)->value) {
      if (c < 0x20 || c >= 0x7f || c == '"' || c == '\\') {
        return "";
      }
      text += static_cast<char>(c);
    }
    return text + "\"";
  }
  case Expr::Kind::ArrayLit:
    return cast<ArrayLitExpr>(expr)->elements.empty() ? "[]" : "";
  case Expr::Kind::OptionalChain:
    return TruthChecker::sourceText(cast<OptionalChainExpr>(expr)->chain);
  case Expr::Kind::Member: {
    auto *member = cast<MemberExpr>(expr);
    std::string object = TruthChecker::sourceText(member->object);
    return object.empty() ? "" : object + (member->optional ? "?." : ".") + member->name.str();
  }
  case Expr::Kind::Index: {
    auto *index = cast<IndexExpr>(expr);
    std::string object = TruthChecker::sourceText(index->object);
    std::string position = TruthChecker::sourceText(index->index);
    if (object.empty() || position.empty()) {
      return "";
    }
    return object + (index->optional ? "?.[" : "[") + position + "]";
  }
  case Expr::Kind::Call: {
    auto *call = cast<CallExpr>(expr);
    std::string text = TruthChecker::sourceText(call->callee);
    if (text.empty() || call->hasSpread()) {
      return "";
    }
    text += call->optional ? "?.(" : "(";
    for (size_t i = 0; i < call->args.size(); ++i) {
      std::string arg = TruthChecker::sourceText(call->args[i]);
      if (arg.empty()) {
        return "";
      }
      text += (i > 0 ? ", " : "") + arg;
    }
    return text + ")";
  }
  default:
    return "";
  }
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

  return member && member->name == "length";
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

TruthChecker::TruthChecker(SemanticState &owner)
    : state(owner) {}

Expr *TruthChecker::checkCondition(Expr *cond, Flow &whenTrue, Flow &whenFalse) {
  return this->checkBranching(cond, whenTrue, whenFalse, TruthUse::Condition);
}

Expr *TruthChecker::checkBranching(Expr *expr, Flow &whenTrue, Flow &whenFalse, TruthUse use) {
  if (auto *bin = dyn_cast<BinaryExpr>(expr);
      bin && (bin->op == BinaryOp::LogAnd || bin->op == BinaryOp::LogOr)) {
    bool isAnd = bin->op == BinaryOp::LogAnd;
    Flow lhsTrue, lhsFalse, rhsTrue, rhsFalse;
    bin->lhs = this->checkBranching(bin->lhs, lhsTrue, lhsFalse);
    if (std::optional<bool> constant = this->state.getGenericStore().isCheckingInstance()
                                           ? GenericChecker(this->state).constantCondition(bin->lhs)
                                           : std::nullopt;
        constant && *constant != isAnd) {
      whenTrue = lhsTrue;
      whenFalse = lhsFalse;
      this->state.getFlowState().replaceFlow(FlowChecker::join(whenTrue, whenFalse));

      return bin->lhs;
    }
    this->state.getFlowState().replaceFlow(isAnd ? lhsTrue : lhsFalse);
    bin->rhs = this->checkBranching(bin->rhs, rhsTrue, rhsFalse);
    whenTrue = isAnd ? rhsTrue : FlowChecker::join(lhsTrue, rhsTrue);
    whenFalse = isAnd ? FlowChecker::join(lhsFalse, rhsFalse) : rhsFalse;
    this->state.getFlowState().replaceFlow(FlowChecker::join(whenTrue, whenFalse));
    JsonChecker(this->state).checkRedundantTests(bin);
    bin->type = Type::boolTy();
    return bin;
  }
  if (auto *unary = dyn_cast<UnaryExpr>(expr); unary && unary->op == UnaryOp::Not) {
    Flow operandTrue, operandFalse;
    unary->operand = this->checkBranching(unary->operand, operandTrue, operandFalse, TruthUse::Not);
    whenTrue = operandFalse;
    whenFalse = operandTrue;
    unary->type = Type::boolTy();
    return unary;
  }
  if (this->isBooleanCall(expr)) {
    return this->checkBooleanCall(cast<CallExpr>(expr), whenTrue, whenFalse);
  }
  if (auto *assign = dyn_cast<AssignExpr>(expr); assign && !assign->op) {
    expr = ExprChecker(this->state).checkExpr(assign);
    whenTrue = this->state.getFlowState().getFlow();
    whenFalse = this->state.getFlowState().getFlow();
    ResultConditionForm form = TruthChecker::assignmentCondition(assign, use);
    if (form.isErr()) {
      this->state.getDiagnostics().error(assign->loc(), form.error().message);
    }
    return TypeRelations(this->state).convert(expr, Type::boolTy());
  }

  expr = ExprChecker(this->state).checkExpr(expr, Type::boolTy());
  whenTrue = this->state.getFlowState().getFlow();
  whenFalse = this->state.getFlowState().getFlow();
  if (auto *lit = dyn_cast<BoolLitExpr>(expr)) {
    (lit->value ? whenFalse : whenTrue).unreachable = true;
  } else {
    ConditionFacts(this->state).addConditionFacts(expr, whenTrue, whenFalse);
  }
  if (!expr->type->isBool()) {
    ResultTruthIsTested truthy = this->checkTruthy(expr, use);
    if (truthy.isErr()) {
      this->state.getDiagnostics().error(expr->loc(), truthy.error().message);
    }
  }

  return TypeRelations(this->state).convert(expr, Type::boolTy());
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
  expr = ExprChecker(this->state).checkExpr(expr, expected);
  whenTrue = this->state.getFlowState().getFlow();
  whenFalse = this->state.getFlowState().getFlow();
  if (auto *lit = dyn_cast<BoolLitExpr>(expr)) {
    (lit->value ? whenFalse : whenTrue).unreachable = true;
  } else {
    ConditionFacts(this->state).addConditionFacts(expr, whenTrue, whenFalse);
  }

  return expr;
}

Expr *TruthChecker::checkLogical(BinaryExpr *bin, const Type *expected, Flow &whenTrue,
                                 Flow &whenFalse, bool tested, TruthUse use) {
  bool isAnd = bin->op == BinaryOp::LogAnd;
  bool isAssign = use == TruthUse::OrAssign || use == TruthUse::AndAssign;
  const char *op = isAnd ? (isAssign ? "&&=" : "&&") : (isAssign ? "||=" : "||");
  const Expr *other = isAssign ? cast<AssignExpr>(bin->rhs)->value : bin->rhs;

  Flow lhsTrue, lhsFalse, rhsTrue, rhsFalse;
  bin->lhs = GenericChecker(this->state)
                 .apparent(this->checkTestedValue(bin->lhs, expected, lhsTrue, lhsFalse, use));
  ResultTruthIsTested leftTruthy = this->checkTruthy(bin->lhs, use, other);
  if (leftTruthy.isErr()) {
    this->state.getDiagnostics().error(bin->lhs->loc(), leftTruthy.error().message);
  }
  bool truthy = leftTruthy.isOk();
  const Type *l = bin->lhs->type;
  this->state.getFlowState().replaceFlow(isAnd ? lhsTrue : lhsFalse);
  const Type *hint = isAnd || l->isError() || l->isUndefined() ? expected : l->nonOptional();
  if (tested) {
    bin->rhs = this->checkTestedValue(bin->rhs, hint, rhsTrue, rhsFalse, TruthUse::Condition);
    ResultTruthIsTested rightTruthy = this->checkTruthy(bin->rhs, TruthUse::Condition);
    if (rightTruthy.isErr()) {
      this->state.getDiagnostics().error(bin->rhs->loc(), rightTruthy.error().message);
    }
    truthy &= rightTruthy.isOk();
  } else {
    bin->rhs = ExprChecker(this->state).checkExpr(bin->rhs, hint);
    rhsTrue = this->state.getFlowState().getFlow();
    rhsFalse = this->state.getFlowState().getFlow();
  }
  whenTrue = isAnd ? rhsTrue : FlowChecker::join(lhsTrue, rhsTrue);
  whenFalse = isAnd ? FlowChecker::join(lhsFalse, rhsFalse) : rhsFalse;
  this->state.getFlowState().replaceFlow(FlowChecker::join(whenTrue, whenFalse));
  if (!isAssign) {
    JsonChecker(this->state).checkRedundantTests(bin);
  }

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
  bin->rhs = TypeRelations(this->state).convert(bin->rhs, result);
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
  bool negated = use == TruthUse::Not;
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

  return head + ErrorTextTruthy::undefTest(x, negated);
}

std::string TruthChecker::jsonMessage(TruthUse use, llvm::StringRef x, llvm::StringRef y) {
  bool negated = use == TruthUse::Not;
  std::string head =
      ErrorTextTruthy::jsonHead(TruthChecker::useOperator(use)) + ErrorTextTruthy::jsonMay();
  if (use == TruthUse::Or) {
    return head + ErrorTextTruthy::jsonOr(x, y);
  }
  if (use == TruthUse::OrAssign) {
    return head + ErrorTextTruthy::orAssign(x, y);
  }

  return head + ErrorTextTruthy::jsonTest(x, negated);
}

std::string TruthChecker::unknownMessage(TruthUse use, llvm::StringRef x, llvm::StringRef y) {
  bool negated = use == TruthUse::Not;
  std::string head =
      ErrorTextTruthy::unknownHd(TruthChecker::useOperator(use)) + ErrorTextTruthy::unknownMay();
  if (use == TruthUse::Or) {
    return head + ErrorTextTruthy::orDefault(x, y);
  }
  if (use == TruthUse::OrAssign) {
    return head + ErrorTextTruthy::orAssign(x, y);
  }

  return head + ErrorTextTruthy::unknownTst(x, negated);
}

std::string TruthChecker::numberMessage(const Expr *expr, const Type *type, const Type *base,
                                        TruthUse use, llvm::StringRef x, llvm::StringRef y) {
  bool negated = use == TruthUse::Not;
  bool length = TruthChecker::isLength(expr);
  std::string what = ErrorTextTruthy::number(base->isDecimal(), type->isOptional());
  std::string head =
      ErrorTextTruthy::cannotTest(TruthChecker::useOperator(use), what) + ErrorTextTruthy::zero();
  if (type->isOptional()) {
    head += ErrorTextTruthy::too();
    bool lengthUse =
        length && (use == TruthUse::Condition || use == TruthUse::Not || use == TruthUse::And);
    if (lengthUse) {
      return head + ErrorTextTruthy::lengthTest(x, negated);
    }
    if (use == TruthUse::Or) {
      return head + ErrorTextTruthy::orDefault(x, y);
    }
    if (use == TruthUse::OrAssign) {
      return head + ErrorTextTruthy::orAssign(x, y);
    }
    return head + ErrorTextTruthy::undefTest(x, negated);
  }
  if (base->isFloat()) {
    return head + ErrorTextTruthy::floatTest(x, negated);
  }

  std::string test = ErrorTextTruthy::zeroTest(x, negated, length);
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
  bool negated = use == TruthUse::Not;
  const NameExpr *narrowed = TruthChecker::narrowedVariable(expr);
  bool isArray = base->isArray();
  bool isPromise = base->isPromise();
  bool isClass = base->isClassRef();
  bool isObject = base->isObject() || base->isIterator() || base->isDict() || isClass || isPromise;
  const Expr *bare = expr;
  while (isa<CastExpr>(bare) && cast<CastExpr>(bare)->isImplicit()) {
    bare = cast<CastExpr>(bare)->operand;
  }
  auto *name = dyn_cast<NameExpr>(bare);
  bool className = isClass && name && name->cls() && !name->var();
  std::string what = ErrorTextTruthy::kind(isArray, isPromise, isClass, isObject);
  std::string message;
  if (narrowed) {
    message = ErrorTextTruthy::holds(narrowed->name, what);
  } else if (isArray) {
    message = ErrorTextTruthy::arrayTrue();
  } else {
    message = ErrorTextTruthy::alwaysTrue(what);
  }
  switch (use) {
    case TruthUse::Condition:
      message += ErrorTextTruthy::soAlways();
      break;
    case TruthUse::Not:
      message += ErrorTextTruthy::soNot();
      break;
    case TruthUse::Or:
      message += ErrorTextTruthy::soOr();
      break;
    case TruthUse::And:
      message += ErrorTextTruthy::soAnd();
      break;
    case TruthUse::OrAssign:
      message += ErrorTextTruthy::soOrSet();
      break;
    case TruthUse::AndAssign:
      message += ErrorTextTruthy::soAndSet();
      break;
  }
  std::string length = ErrorTextTruthy::lengthCmp(x, negated);
  std::string call = isObject ? "" : ErrorTextTruthy::callOf(x, negated, base->minArgs() == 0);
  if (use == TruthUse::AndAssign) {
    return message + ErrorTextTruthy::useAssign();
  }
  if (use == TruthUse::Or) {
    return message + ErrorTextTruthy::dropOr(y);
  }
  if (use == TruthUse::OrAssign) {
    return message + (isArray ? ErrorTextTruthy::arrayLen(x, y) : ErrorTextTruthy::dropAssign());
  }
  if ((narrowed || isObject) && use == TruthUse::And) {
    return message + ErrorTextTruthy::dropAnd(x);
  }
  if (narrowed || className) {
    return message + (isArray ? ErrorTextTruthy::dropOrLen(length) : ErrorTextTruthy::dropCheck());
  }
  if (isArray) {
    return message + ErrorTextTruthy::checkLen(length);
  }
  if (isObject) {
    return message + ErrorTextTruthy::allowUndef(base->name());
  }

  return message + ErrorTextTruthy::callIt(call);
}

TruthChecker::ResultTruthIsTested TruthChecker::checkTruthy(const Expr *expr, TruthUse use,
                                                            const Expr *other) {
  const Type *type = expr->type;
  const Type *base = GenericChecker::apparentType(type->nonOptional());
  if (type->isUndefined() || base->isNever()) {
    return ok(expr);
  }
  if (type->isError() || base->isBool() || base->isString() ||
      (type->isOptional() &&
       (base->isArray() || base->isFunction() || base->isObject() || base->isResult() ||
        base->isIterator() || base->isDict() || base->isPromise() || base->isClassRef()))) {
    return ok(expr);
  }
  auto boolOrString = [](const Type *t) { return t->isBool() || t->isString(); };
  if (base->isParam() && !base->alternatives().empty() &&
      llvm::all_of(base->alternatives(), boolOrString)) {
    return ok(expr);
  }
  if (base->isUnion() || base->isLiteral()) {
    llvm::ArrayRef<const Type *> members =
        base->isUnion() ? base->members() : llvm::ArrayRef<const Type *>(base);
    const Type *number = nullptr;
    bool falsy = type->isOptional();
    for (const Type *m : members) {
      const Type *b = m->isLiteral() ? m->literalBase() : m;
      if (b->isNumeric() || b->isDecimal() || b->isParam() || b->isJson()) {
        number = m;
      }
      bool empty = m->isStringLiteral() ? m->literalText().empty() : m->literalNumber() == 0;
      if (m->isLiteral() ? empty : b->isBool() || b->isString()) {
        falsy = true;
      }
    }
    if (!number && falsy) {
      return ok(expr);
    }
    if (!number) {
      std::string message = ErrorTextTruthy::unionTrue(type->name());

      return err(TruthCheckerCheckTruthyError{
          .code = TruthCheckerCheckTruthyErrorCode::UNION_ALWAYS_TRUTHY,
          .message = message,
      });
    }
    std::string value = TruthChecker::hintText(expr);
    std::string message = ErrorTextTruthy::unionNum(type->name(), number->name(), value);

    return err(TruthCheckerCheckTruthyError{
        .code = TruthCheckerCheckTruthyErrorCode::UNION_WITH_NUMBER,
        .message = message,
    });
  }

  bool negated = use == TruthUse::Not;
  std::string x = TruthChecker::hintText(expr);
  std::string y = TruthChecker::hintText(other, "...");
  if (type->isVoid()) {
    std::string message = ErrorTextTruthy::voidValue();

    return err(TruthCheckerCheckTruthyError{
        .code = TruthCheckerCheckTruthyErrorCode::VOID_VALUE,
        .message = message,
    });
  }
  if (type->isUndefined()) {
    std::string message = ErrorTextTruthy::undefined();

    return err(TruthCheckerCheckTruthyError{
        .code = TruthCheckerCheckTruthyErrorCode::ALWAYS_UNDEFINED,
        .message = message,
    });
  }
  if (base->isParam()) {
    std::string message = TruthChecker::paramMessage(type, base, use, x, y);

    return err(TruthCheckerCheckTruthyError{
        .code = TruthCheckerCheckTruthyErrorCode::PARAM_MAY_BE_NUMBER,
        .message = message,
    });
  }
  if (base->isJson()) {
    std::string message = TruthChecker::jsonMessage(use, x, y);

    return err(TruthCheckerCheckTruthyError{
        .code = TruthCheckerCheckTruthyErrorCode::JSON_VALUE,
        .message = message,
    });
  }
  if (base->isUnknown()) {
    std::string message = TruthChecker::unknownMessage(use, x, y);

    return err(TruthCheckerCheckTruthyError{
        .code = TruthCheckerCheckTruthyErrorCode::UNKNOWN_VALUE,
        .message = message,
    });
  }
  if (base->isResult()) {
    std::string message = ErrorTextTruthy::result(x, negated);

    return err(TruthCheckerCheckTruthyError{
        .code = TruthCheckerCheckTruthyErrorCode::RESULT_VALUE,
        .message = message,
    });
  }
  if (base->isNumeric() || base->isDecimal()) {
    std::string message = TruthChecker::numberMessage(expr, type, base, use, x, y);

    return err(TruthCheckerCheckTruthyError{
        .code = TruthCheckerCheckTruthyErrorCode::NUMBER_VALUE,
        .message = message,
    });
  }

  std::string message = TruthChecker::objectMessage(expr, base, use, x, y);

  return err(TruthCheckerCheckTruthyError{
      .code = TruthCheckerCheckTruthyErrorCode::ALWAYS_TRUTHY,
      .message = message,
  });
}

TruthChecker::ResultConditionForm TruthChecker::assignmentCondition(const AssignExpr *assign,
                                                                    TruthUse use) {
  bool negated = use == TruthUse::Not;
  std::string target = TruthChecker::hintText(assign->target);
  std::string value = TruthChecker::hintText(assign->value, "...");
  const Type *type = assign->type;
  bool compareInstead =
      TruthChecker::isLiteral(assign->value) || type->isError() ||
      !(type->isOptional() || type->isNumeric() || type->isDecimal() || type->isString());
  if (compareInstead) {
    std::string eq = negated ? "!=" : "==";
    std::string message = ErrorTextAssignTest::compare(eq, target, value);

    return err(TruthCheckerAssignmentConditionError{
        .code = TruthCheckerAssignmentConditionErrorCode::COMPARE_INSTEAD,
        .message = message,
    });
  }

  std::string message =
      ErrorTextAssignTest::testValue(target, value, negated, type->isOptional(), type->isString());

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
         CallChecker(this->state).isGlobalName(call->callee, "Boolean");
}

Expr *TruthChecker::checkBooleanCall(CallExpr *call, Flow &whenTrue, Flow &whenFalse) {
  return this->checkBranching(call->args[0], whenTrue, whenFalse);
}

}
