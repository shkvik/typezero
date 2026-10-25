// Truthiness: what a condition may test, and what `!`, `&&`, `||`, `||=`,
// `&&=` and Boolean(x) make of values other than booleans.
//
// As in JavaScript, a condition may test a string (is it empty?) and a value
// that may be undefined (is it there?). A number may not be tested: 0 is an
// ordinary value of a count or a port, NaN would be false too, and
// `port || 3000` would lose a zero, so the errors suggest `!= 0` or `??`. An
// array or a function is always truthy, which makes testing one pointless:
// that is an error as well.
//
// A tested value becomes a boolean through a conversion (a CastExpr), which
// CodeGen turns into a check without calls. `a && b` and `a || b` used as
// values give one of their operands, as in JavaScript: with `nick` of type
// `string | undefined`, `nick || "anon"` is a string.

#include "compiler/check/sema/sema.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

// The source text of a simple expression, for hints: names, literals, and
// members, elements and calls of those. Empty for anything else.
std::string sourceText(const Expr *expr) {
  if (auto *conv = dyn_cast<CastExpr>(expr); conv && conv->isImplicit())
    return sourceText(conv->operand);
  switch (expr->kind()) {
  case Expr::Kind::Name: {
    llvm::StringRef name = cast<NameExpr>(expr)->name;
    return name.starts_with("%") ? "" : name.str(); // a hidden variable
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
      if (c < 0x20 || c >= 0x7f || c == '"' || c == '\\')
        return "";
      text += static_cast<char>(c);
    }
    return text + "\"";
  }
  case Expr::Kind::ArrayLit:
    return cast<ArrayLitExpr>(expr)->elements.empty() ? "[]" : "";
  case Expr::Kind::OptionalChain:
    return sourceText(cast<OptionalChainExpr>(expr)->chain);
  case Expr::Kind::Member: {
    auto *member = cast<MemberExpr>(expr);
    std::string object = sourceText(member->object);
    return object.empty() ? "" : object + (member->optional ? "?." : ".") + member->name.str();
  }
  case Expr::Kind::Index: {
    auto *index = cast<IndexExpr>(expr);
    std::string object = sourceText(index->object), position = sourceText(index->index);
    if (object.empty() || position.empty())
      return "";
    return object + (index->optional ? "?.[" : "[") + position + "]";
  }
  case Expr::Kind::Call: {
    auto *call = cast<CallExpr>(expr);
    std::string text = sourceText(call->callee);
    if (text.empty() || call->hasSpread())
      return "";
    text += call->optional ? "?.(" : "(";
    for (size_t i = 0; i < call->args.size(); ++i) {
      std::string arg = sourceText(call->args[i]);
      if (arg.empty())
        return "";
      text += (i > 0 ? ", " : "") + arg;
    }
    return text + ")";
  }
  default:
    return "";
  }
}

// The text of an expression in a hint: `x` or `...` where it is too complex
// to show.
std::string hintText(const Expr *expr, const char *placeholder = "x") {
  std::string text = expr ? sourceText(expr) : "";
  return text.empty() ? placeholder : text;
}

// `x` where x is a `T | undefined` variable known to hold a value here.
const NameExpr *narrowedVariable(const Expr *expr) {
  auto *conv = dyn_cast<CastExpr>(expr);
  if (!conv || !conv->isImplicit() || conv->shortCircuit || !conv->operand->type->isOptional() ||
      conv->type != conv->operand->type->inner())
    return nullptr;
  return dyn_cast<NameExpr>(conv->operand);
}

bool isLength(const Expr *expr) {
  for (;;) {
    if (auto *conv = dyn_cast<CastExpr>(expr))
      expr = conv->operand;
    else if (auto *chain = dyn_cast<OptionalChainExpr>(expr))
      expr = chain->chain;
    else
      break;
  }
  auto *member = dyn_cast<MemberExpr>(expr);
  return member && member->name == "length";
}

bool isLiteral(const Expr *expr) {
  while (auto *conv = dyn_cast<CastExpr>(expr))
    expr = conv->operand;
  if (auto *unary = dyn_cast<UnaryExpr>(expr); unary && unary->op == UnaryOp::Neg)
    expr = unary->operand;
  return isa<IntLitExpr>(expr) || isa<FloatLitExpr>(expr) || isa<StringLitExpr>(expr) ||
         isa<BoolLitExpr>(expr) || isa<UndefinedExpr>(expr);
}

// What `a && b` gives when a is falsy: a itself, which is undefined if it is
// an array, a function, an object or a class.
const Type *falsyType(const Type *type) {
  const Type *base = type->nonOptional();
  return base->isArray() || base->isFunction() || base->isObject() || base->isIterator() ||
                 base->isDict() || base->isClassRef()
             ? Type::undefinedTy()
             : type;
}

} // namespace

Expr *Sema::checkCondition(Expr *cond, Flow &whenTrue, Flow &whenFalse) {
  return checkBranching(cond, whenTrue, whenFalse, TruthUse::Condition);
}

// Checks an expression whose truth matters, following `&&`, `||` and `!` so
// that the right side of `i < a.length && a[i] > 0` knows `i < a.length`. Only
// the truth of the result is used, so every operand becomes a boolean.
Expr *Sema::checkBranching(Expr *expr, Flow &whenTrue, Flow &whenFalse, TruthUse use) {
  if (auto *bin = dyn_cast<BinaryExpr>(expr);
      bin && (bin->op == BinaryOp::LogAnd || bin->op == BinaryOp::LogOr)) {
    bool isAnd = bin->op == BinaryOp::LogAnd;
    Flow lhsTrue, lhsFalse, rhsTrue, rhsFalse;
    bin->lhs = checkBranching(bin->lhs, lhsTrue, lhsFalse);
    // `typeof x == "string" && x.length > 0` in an instance where x is a
    // number: the right side never runs, and was written for other types.
    if (std::optional<bool> constant = checkingInstance_ ? constantCondition(bin->lhs) : std::nullopt;
        constant && *constant != isAnd) {
      whenTrue = lhsTrue;
      whenFalse = lhsFalse;
      flow_ = join(whenTrue, whenFalse);
      return bin->lhs;
    }
    flow_ = isAnd ? lhsTrue : lhsFalse; // the right side runs only then
    bin->rhs = checkBranching(bin->rhs, rhsTrue, rhsFalse);
    whenTrue = isAnd ? rhsTrue : join(lhsTrue, rhsTrue);
    whenFalse = isAnd ? join(lhsFalse, rhsFalse) : rhsFalse;
    flow_ = join(whenTrue, whenFalse);
    bin->type = Type::boolTy();
    return bin;
  }
  if (auto *unary = dyn_cast<UnaryExpr>(expr); unary && unary->op == UnaryOp::Not) {
    Flow operandTrue, operandFalse;
    unary->operand = checkBranching(unary->operand, operandTrue, operandFalse, TruthUse::Not);
    whenTrue = operandFalse;
    whenFalse = operandTrue;
    unary->type = Type::boolTy();
    return unary;
  }
  if (isBooleanCall(expr))
    return checkBooleanCall(cast<CallExpr>(expr), whenTrue, whenFalse);
  // `if (level = 0)`, also in double parentheses, which JavaScript linters
  // take as "I mean it".
  if (auto *assign = dyn_cast<AssignExpr>(expr); assign && !assign->op) {
    expr = checkExpr(assign);
    whenTrue = flow_;
    whenFalse = flow_;
    reportAssignmentCondition(assign, use);
    return convert(expr, Type::boolTy());
  }

  expr = checkExpr(expr, Type::boolTy());
  whenTrue = flow_;
  whenFalse = flow_;
  if (auto *lit = dyn_cast<BoolLitExpr>(expr))
    (lit->value ? whenFalse : whenTrue).unreachable = true;
  else
    addConditionFacts(expr, whenTrue, whenFalse);
  if (!expr->type->isBool())
    checkTruthy(expr, use);
  // A boolean even after an error, so that nothing else is reported.
  return convert(expr, Type::boolTy());
}

Expr *Sema::checkTestedValue(Expr *expr, const Type *expected, Flow &whenTrue, Flow &whenFalse,
                             TruthUse use) {
  if (auto *bin = dyn_cast<BinaryExpr>(expr);
      bin && (bin->op == BinaryOp::LogAnd || bin->op == BinaryOp::LogOr))
    return checkLogical(bin, expected, whenTrue, whenFalse, /*tested=*/true,
                        bin->op == BinaryOp::LogAnd ? TruthUse::And : TruthUse::Or);
  // A boolean anyway.
  if (auto *unary = dyn_cast<UnaryExpr>(expr); (unary && unary->op == UnaryOp::Not) || isBooleanCall(expr))
    return checkBranching(expr, whenTrue, whenFalse, use);
  expr = checkExpr(expr, expected);
  whenTrue = flow_;
  whenFalse = flow_;
  if (auto *lit = dyn_cast<BoolLitExpr>(expr))
    (lit->value ? whenFalse : whenTrue).unreachable = true;
  else
    addConditionFacts(expr, whenTrue, whenFalse);
  return expr;
}

// `a || b` gives a if it is truthy, else b; `a && b` gives a if it is falsy,
// else b. b runs only when a does not decide. The result has the type that
// holds both: `nick || "anon"` is a string, `items && items.length` an
// `int | undefined`. Both booleans give a boolean, as before.
Expr *Sema::checkLogical(BinaryExpr *bin, const Type *expected, Flow &whenTrue, Flow &whenFalse, bool tested,
                         TruthUse use) {
  bool isAnd = bin->op == BinaryOp::LogAnd;
  bool isAssign = use == TruthUse::OrAssign || use == TruthUse::AndAssign;
  const char *op = isAnd ? (isAssign ? "&&=" : "&&") : (isAssign ? "||=" : "||");
  // The value the hints about the left side mention: what `||=` assigns.
  const Expr *other = isAssign ? cast<AssignExpr>(bin->rhs)->value : bin->rhs;

  Flow lhsTrue, lhsFalse, rhsTrue, rhsFalse;
  // `T extends string` is tested, and given, as a string.
  bin->lhs = apparent(checkTestedValue(bin->lhs, expected, lhsTrue, lhsFalse, use));
  bool ok = checkTruthy(bin->lhs, use, other);
  const Type *l = bin->lhs->type;
  flow_ = isAnd ? lhsTrue : lhsFalse; // the right side runs only then
  // Like `??`, `||` gives the right side where the left would be: an empty
  // array literal there takes the type of the left side.
  const Type *hint = isAnd || l->isError() || l->isUndefined() ? expected : l->nonOptional();
  if (tested) {
    bin->rhs = checkTestedValue(bin->rhs, hint, rhsTrue, rhsFalse, TruthUse::Condition);
    ok &= checkTruthy(bin->rhs, TruthUse::Condition);
  } else {
    bin->rhs = checkExpr(bin->rhs, hint);
    rhsTrue = flow_;
    rhsFalse = flow_;
  }
  whenTrue = isAnd ? rhsTrue : join(lhsTrue, rhsTrue);
  whenFalse = isAnd ? join(lhsFalse, rhsFalse) : rhsFalse;
  flow_ = join(whenTrue, whenFalse);

  const Type *r = bin->rhs->type;
  bin->type = Type::error();
  if (!ok)
    return bin;
  // The value of the left side when it decides.
  const Type *kept = l->isError() ? l : isAnd ? falsyType(l) : l->nonOptional();
  if (l->isError() || r->isError()) {
    // A call in a cycle may have no type yet (see inferCycle): the other
    // side tells.
    if (trial_ && l->isError() != r->isError())
      bin->type = l->isError() ? r : kept;
    return bin;
  }
  if (l->isBool() && r->isBool()) {
    bin->type = l;
    return bin;
  }
  if (r->isVoid()) {
    std::string test = hintText(bin->lhs);
    diags_.error(bin->loc(), llvm::Twine("the right side of '") + op +
                                 "' returns nothing, so there is no value to give; call it with 'if': 'if (" +
                                 (isAnd ? "" : "!") + test + ") " + hintText(bin->rhs, "...") + "'");
    return bin;
  }
  const Type *result = unify(kept, r);
  if (!result) {
    diags_.error(bin->loc(), llvm::Twine("'") + op + "' gives '" + kept->name() + "' when its left side is " +
                                 (isAnd ? "falsy" : "truthy") + " and '" + r->name() +
                                 "' otherwise, and no type holds both; say what each case gives with '?:'");
    return bin;
  }
  bin->rhs = convert(bin->rhs, result);
  bin->type = result;
  return bin;
}

bool Sema::checkTruthy(const Expr *expr, TruthUse use, const Expr *other) {
  // A type parameter tests as its constraint: `T extends string` as a string.
  const Type *type = expr->type, *base = apparentType(type->nonOptional());
  // Nothing but undefined, as an optional parameter of a type `never` in a copy of a generic
  // function (`buf?: TBuf` with TBuf = never): always false, which the copy may well test.
  if (type->isUndefined() || base->isNever())
    return true;
  if (type->isError() || base->isBool() || base->isString() ||
      (type->isOptional() &&
       (base->isArray() || base->isFunction() || base->isObject() || base->isResult() ||
        base->isIterator() || base->isDict() || base->isPromise() || base->isClassRef())))
    return true;
  // `T extends string | boolean`: none of them is a number.
  if (base->isParam() && !base->alternatives().empty() &&
      llvm::all_of(base->alternatives(), [](const Type *t) { return t->isBool() || t->isString(); }))
    return true;
  // A union or a literal type: if no member is a number, and some member
  // (or undefined) may be falsy.
  if (base->isUnion() || base->isLiteral()) {
    llvm::ArrayRef<const Type *> members =
        base->isUnion() ? base->members() : llvm::ArrayRef<const Type *>(base);
    const Type *number = nullptr;
    bool falsy = type->isOptional();
    for (const Type *m : members) {
      const Type *b = m->isLiteral() ? m->literalBase() : m;
      if (b->isNumeric() || b->isDecimal() || b->isParam() || b->isJson())
        number = m;
      bool empty = m->isStringLiteral() ? m->literalText().empty() : m->literalNumber() == 0;
      if (m->isLiteral() ? empty : b->isBool() || b->isString())
        falsy = true;
    }
    if (!number && falsy)
      return true;
    if (!number) {
      diags_.error(expr->loc(), "a value of type '" + type->name().str() +
                                    "' is always truthy, so the condition is always true");
      return false;
    }
    diags_.error(expr->loc(), "a value of type '" + type->name().str() +
                                  "' cannot be a condition: '" + number->name().str() +
                                  "' is a number, and 0 would count as false; check what it "
                                  "holds, e.g. 'typeof " +
                                  hintText(expr) + " == \"string\"'");
    return false;
  }

  const char *op = nullptr;
  switch (use) {
  case TruthUse::Condition: break;
  case TruthUse::Not: op = "!"; break;
  case TruthUse::Or: op = "||"; break;
  case TruthUse::And: op = "&&"; break;
  case TruthUse::OrAssign: op = "||="; break;
  case TruthUse::AndAssign: op = "&&="; break;
  }
  bool negated = use == TruthUse::Not;
  std::string x = hintText(expr), y = hintText(other, "...");
  std::string message;

  if (type->isVoid() || type->isUndefined()) {
    message = type->isVoid() ? "an expression of type 'void' has no value to test"
                             : "the value is always undefined, which is always falsy";
  } else if (base->isParam()) {
    // An instance may make it a number, where `firstOr([0, 5], 9)` would
    // give 9.
    std::string what = "a value of type '" + type->name().str() + "'";
    message = op ? "'" + std::string(op) + "' cannot test " + what : what + " cannot be a condition";
    message += ": '" + base->name().str() + "' may be a number, and 0 would count as false; ";
    if (use == TruthUse::Or)
      message += "give a default with '??': '" + x + " ?? " + y + "'";
    else if (use == TruthUse::OrAssign)
      message += "give a default with '??=': '" + x + " ??= " + y + "'";
    else if (use == TruthUse::AndAssign)
      message += "check for undefined: 'if (" + x + " != undefined) " + x + " = " + y + "'";
    else
      message += "check for undefined: '" + x + (negated ? " == " : " != ") + "undefined'";
  } else if (base->isJson()) {
    // Any value of JavaScript but undefined: 0, "", false and null are false.
    message = op ? "'" + std::string(op) + "' cannot test a Json" : "a Json cannot be a condition";
    message += ": it may hold 0, \"\", false or null, which would count as false; ";
    if (use == TruthUse::Or)
      message += "give a default with '??', which replaces null and undefined: '" + x + " ?? " + y +
                 "'";
    else if (use == TruthUse::OrAssign)
      message += "give a default with '??=': '" + x + " ??= " + y + "'";
    else
      message += "compare it as you mean, e.g. '" + x + (negated ? " == null'" : " != null'") +
                 ", '" + x + (negated ? " != true'" : " == true'") + " or 'typeof " + x +
                 " == \"string\"'";
  } else if (base->isUnknown()) {
    // It may hold a number, where 0 would count as false (docs/small.md).
    message = op ? "'" + std::string(op) + "' cannot test an unknown value"
                 : "an unknown value cannot be a condition";
    message += ": it may hold 0, \"\" or false, which would count as false; ";
    if (use == TruthUse::Or)
      message += "give a default with '??': '" + x + " ?? " + y + "'";
    else if (use == TruthUse::OrAssign)
      message += "give a default with '??=': '" + x + " ??= " + y + "'";
    else
      message += "check what it holds, e.g. '" + x + (negated ? " == undefined'" : " != undefined'") +
                 " or 'typeof " + x + " == \"string\"'";
  } else if (base->isResult()) {
    // An Err is an object too: `if (save())` would never see the error.
    message = "a Result is always truthy, an Err too, as both are objects in JavaScript; test "
              "it: '" + x + (negated ? ".isErr()'" : ".isOk()'");
  } else if (base->isNumeric() || base->isDecimal()) {
    std::string noun = base->isDecimal() ? "a decimal" : "a number";
    std::string what = type->isOptional() ? noun + " that may be undefined" : noun;
    message = op ? "'" + std::string(op) + "' cannot test " + what : what + " cannot be a condition";
    message += base->isFloat() ? ": 0 and NaN would count as false" : ": 0 would count as false";
    if (type->isOptional()) {
      message += " too; ";
      // `if (items?.length)` asks whether there are any.
      if (isLength(expr) && (use == TruthUse::Condition || use == TruthUse::Not || use == TruthUse::And))
        message += "compare it: '(" + x + " ?? 0) " + (negated ? "== 0'" : "> 0'");
      else if (use == TruthUse::Or)
        message += "give a default with '??': '" + x + " ?? " + y + "'";
      else if (use == TruthUse::OrAssign)
        message += "give a default with '??=': '" + x + " ??= " + y + "'";
      else
        message += "check for undefined: '" + x + (negated ? " == " : " != ") + "undefined'";
    } else if (base->isFloat()) {
      // NaN too: the hint makes the choice explicit.
      message += "; compare it as you mean: '" + x + (negated ? " == 0', '" : " != 0', '") + x +
                 (negated ? " <= 0' or 'Number.isNaN(" : " > 0' or '!Number.isNaN(") + x + ")'";
    } else {
      // A length is never negative: `> 0` says what is meant.
      std::string test = negated ? x + " == 0" : isLength(expr) ? x + " > 0" : x + " != 0";
      if (use == TruthUse::Or)
        message += "; compare it: '" + test + " ? " + x + " : " + y + "'";
      else if (use == TruthUse::OrAssign)
        message += "; compare it: 'if (" + x + " == 0) " + x + " = " + y + "'";
      else if (use == TruthUse::AndAssign)
        message += "; compare it: 'if (" + test + ") " + x + " = " + y + "'";
      else
        message += "; compare it: '" + test + "'";
    }
  } else {
    // An array, a function or an object, which is always truthy.
    const NameExpr *narrowed = narrowedVariable(expr);
    bool isArray = base->isArray(), isPromise = base->isPromise(), isClass = base->isClassRef(),
         isObject = base->isObject() || base->isIterator() || base->isDict() || isClass ||
                    isPromise;
    // The name of a class itself, `if (User)`, which no type makes undefined.
    const Expr *bare = expr;
    while (isa<CastExpr>(bare) && cast<CastExpr>(bare)->isImplicit())
      bare = cast<CastExpr>(bare)->operand;
    auto *name = dyn_cast<NameExpr>(bare);
    bool className = isClass && name && name->cls && !name->var;
    const char *what = isArray     ? "an array"
                       : isPromise ? "a Promise"
                       : isClass   ? "a class"
                       : isObject  ? "an object"
                                   : "a function";
    if (narrowed)
      message = "'" + narrowed->name.str() + "' always holds " + what + " at this point";
    else
      message = isArray ? "an array is always truthy, even when empty" : std::string(what) + " is always truthy";
    switch (use) {
    case TruthUse::Condition: message += ", so the condition is always true"; break;
    case TruthUse::Not: message += ", so '!' always gives false"; break;
    case TruthUse::Or: message += ", so '||' never gives its right side"; break;
    case TruthUse::And: message += ", so '&&' always gives its right side"; break;
    case TruthUse::OrAssign: message += ", so '||=' never assigns"; break;
    case TruthUse::AndAssign: message += ", so '&&=' always assigns"; break;
    }
    std::string length = x + (negated ? ".length == 0" : ".length > 0");
    std::string call = isObject ? "" : (negated ? "!" : "") + x + (base->minArgs() == 0 ? "()" : "(...)");
    if (use == TruthUse::AndAssign)
      message += "; use '='";
    else if (use == TruthUse::Or)
      message += "; drop '|| " + y + "'";
    else if (use == TruthUse::OrAssign)
      message += isArray ? "; check its length: 'if (" + x + ".length == 0) " + x + " = " + y + "'"
                         : "; drop the assignment";
    else if ((narrowed || isObject) && use == TruthUse::And)
      message += "; drop '" + x + " &&'";
    else if (narrowed || className)
      message += isArray ? "; drop the check, or check the length: '" + length + "'" : "; drop the check";
    else if (isArray)
      message += "; check its length: '" + length + "'";
    else if (isObject) // a type without `| undefined` says it is always there
      message += "; drop the check, or give it a type that allows undefined: '" + base->name().str() +
                 " | undefined'";
    else
      message += "; call it: '" + call + "'";
  }
  diags_.error(expr->loc(), message);
  return false;
}

// `if (level = 0)` is almost always `==` mistyped; `while (line = next())`
// tests the assigned value, and would also stop at an empty line. The hint
// depends on which it looks like.
void Sema::reportAssignmentCondition(const AssignExpr *assign, TruthUse use) {
  bool negated = use == TruthUse::Not;
  std::string target = hintText(assign->target), value = hintText(assign->value, "...");
  const Type *type = assign->type;
  std::string message = "an assignment cannot be a condition; ";
  if (isLiteral(assign->value) || type->isError() ||
      !(type->isOptional() || type->isNumeric() || type->isDecimal() || type->isString())) {
    const char *eq = negated ? "!=" : "==";
    message += "to compare, use '" + std::string(eq) + "': '" + target + " " + eq + " " + value + "'";
  } else {
    const char *empty = type->isOptional() ? "undefined" : type->isString() ? "\"\"" : "0";
    message += "to test the assigned value, compare it: '(" + target + " = " + value + ") " +
               (negated ? "== " : "!= ") + empty + "'";
  }
  diags_.error(assign->loc(), message);
}

bool Sema::isBooleanCall(const Expr *expr) const {
  auto *call = dyn_cast<CallExpr>(expr);
  return call && !call->optional && call->args.size() == 1 && !call->hasSpread() &&
         isGlobalName(call->callee, "Boolean");
}

// `Boolean(x)` is `!!x`: x is tested as a condition would test it, and
// narrows the same way.
Expr *Sema::checkBooleanCall(CallExpr *call, Flow &whenTrue, Flow &whenFalse) {
  return checkBranching(call->args[0], whenTrue, whenFalse);
}

} // namespace tz
