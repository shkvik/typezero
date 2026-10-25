// The type unknown, and `object` (docs/decorators.md, section 4; docs/small.md,
// section 1).
//
// An unknown holds a value of any type, undefined among them: code can store
// it, pass it on, compare it with `==`, print it and test what it holds, and
// nothing else until a check shows what that is. `typeof x == "string"` makes
// an unknown variable a string where it holds, "number" a double, "boolean" a
// boolean and "object" an object; `x instanceof C` makes it an instance of C,
// and `Array.isArray(x)` an `unknown[]`. As in TypeScript, `unknown |
// undefined` is unknown, so `x != undefined` changes no type.
//
// `object` is any object of the language: an instance of a class, an object
// literal, a date, a dictionary, a class itself. It too can only be kept,
// passed on, compared and printed until `instanceof` shows its class. Both
// are references in memory (see codegen_unknown.cpp): an object as it is, and
// in an unknown any other value in a box of the runtime.

#include "sema/sema.h"

#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

// The variable a check tests, through the conversions that narrowing adds;
// null unless it is an unknown or an object.
const VarSymbol *checkedVariable(const Expr *expr) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit())
      break;
    expr = conv->operand;
  }
  auto *name = dyn_cast<NameExpr>(expr);
  if (!name || !name->var || !name->var->type)
    return nullptr;
  const Type *type = name->var->type;
  return type->isUnknown() || type->nonOptional()->isAnyObject() ? name->var : nullptr;
}

// The name of a variable for a hint, or "x".
std::string hintName(const Expr *expr) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit())
      break;
    expr = conv->operand;
  }
  if (auto *name = dyn_cast<NameExpr>(expr))
    return name->name.str();
  return "x";
}

} // namespace

bool Sema::isObjectReference(const Type *type) {
  return type->isObject() || type->isIterator() || type->isDict() || type->isClassRef() ||
         type->isPromise() ||
         // A union of objects only: a reference to one.
         (type->isUnion() && !type->isTagged() && type->slots().front() == UnionSlot::Object);
}

bool Sema::holdsUnknown(const Type *type) {
  while (type->isOptional() || type->isArray())
    type = type->inner();
  return type->isUnknown();
}

std::string Sema::unknownHint(const Expr *expr, const Type *to) {
  std::string x = hintName(expr);
  const Type *target = to ? to->nonOptional() : nullptr;
  bool object = expr->type && expr->type->isAnyObject();
  std::string check;
  if (target && target->isClass() && !target->classDecl()->literal)
    check = x + " instanceof " + target->classDecl()->name.str();
  else if (object)
    check = x + " instanceof C";
  else if (target && target->isString())
    check = "typeof " + x + " == \"string\"";
  else if (target && target->isNumeric())
    check = "typeof " + x + " == \"number\"";
  else if (target && target->isBool())
    check = "typeof " + x + " == \"boolean\"";
  else if (target && target->isArray())
    check = "Array.isArray(" + x + ")";
  else if (target && target->isAnyObject())
    check = "typeof " + x + " == \"object\"";
  else
    check = "typeof " + x + " == \"string\"";
  if (object)
    return "; an object may be of any class: check which first, e.g. 'if (" + check + ") ...'";
  return "; an unknown value may hold anything: check what it holds first, e.g. 'if (" + check +
         ") ...'";
}

void Sema::reportUnknownUse(const Expr *object, const std::string &what) {
  const Type *type = object->type;
  std::string x = hintName(object);
  if (type->isAnyObject()) {
    diags_.error(object->loc(), "an object " + (what.empty() ? "cannot be indexed" : "has no " + what) +
                                    " until 'instanceof' shows its class, e.g. 'if (" + x +
                                    " instanceof C) ...'");
    return;
  }
  std::string example = what.empty() ? "Array.isArray(" + x + ")" : x + " instanceof C";
  diags_.error(object->loc(), "an unknown value " +
                                  (what.empty() ? "cannot be indexed" : "has no " + what) +
                                  " until a check shows what it holds, e.g. 'if (" + example +
                                  ") ...' or 'typeof " + x + " == \"string\"'");
}

Expr *Sema::checkUnknownMember(MemberExpr *member) {
  member->type = Type::error();
  if (member->object->type->isClassRef())
    return checkClassRefMember(member);
  // In TypeScript a decorator of a member gets the prototype, whose
  // constructor is the class; here it gets the class.
  if (member->name == "constructor") {
    diags_.error(member->loc(), "'constructor' is not supported: an object has no prototype in tz; "
                                "a decorator gets the class itself, use it as it is");
    return member;
  }
  reportUnknownUse(member->object, "property '" + member->name.str() + "'");
  return member;
}

Expr *Sema::checkUnknownMethod(CallExpr *call, MemberExpr *member) {
  const Type *receiver = member->object->type;
  if (!receiver->isUnknown() && !receiver->isAnyObject() && !receiver->isClassRef())
    return nullptr;
  call->type = Type::error();
  // A static method of the class a class as a value is, known when
  // compiling; its value is evaluated first.
  if (ClassDecl *cls = const_cast<ClassDecl *>(receiver->classRef()); cls && cls->type) {
    Expr *object = member->object;
    Expr *result = checkClassStaticCall(call, member, cls);
    if (isa<NameExpr>(object))
      return result;
    auto *seq = ctx_.make<SequenceExpr>(call->loc(), std::vector<Expr *>{object, result});
    seq->type = result->type;
    return seq;
  }
  if (receiver->isConstructor())
    diags_.error(member->loc(), "'" + receiver->name() + "' is some class known only when the "
                                "program runs, whose static members are not known; call its "
                                "methods through reflection (docs/decorators.md, section 6)");
  else if (receiver->isClassRef())
    diags_.error(member->loc(), "'" + receiver->classRef()->name + "' as a value has no methods "
                                "in tz");
  else
    reportUnknownUse(member->object, "method '" + member->name.str() + "'");
  for (Expr *&arg : call->args)
    arg = checkExpr(arg);
  return call;
}

void Sema::addUnknownFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const {
  // A variable gets the type a check showed, in place of what an earlier one
  // showed (`typeof x == "object"`, then `x instanceof C`).
  auto narrow = [](Flow &flow, const VarSymbol *var, const Type *type) {
    llvm::erase_if(flow.narrowed, [&](const auto &narrowed) { return narrowed.first == var; });
    flow.narrowed.push_back({var, type});
  };
  if (auto *call = dyn_cast<CallExpr>(cond); call && call->builtin == Builtin::ArrayIsArray) {
    if (const VarSymbol *var = checkedVariable(call->args[0]))
      narrow(whenTrue, var, Type::array(Type::unknown()));
    return;
  }
  auto *bin = dyn_cast<BinaryExpr>(cond);
  if (!bin)
    return;
  if (bin->op == BinaryOp::InstanceOf) {
    const Type *cls = bin->rhs->type;
    // A class known only when the program runs, whose objects are a T.
    if (cls && cls->isConstructor())
      cls = cls->constructed();
    // A class, a Date, or a type of binary data (Buffer for a Uint8Array
    // too: an unknown holds what it holds).
    if (const VarSymbol *var = checkedVariable(bin->lhs);
        var && cls && (cls->isObject() || cls->isParam() || cls->isDate() || cls->isBytes()))
      narrow(whenTrue, var, cls);
    return;
  }
  if (bin->op != BinaryOp::Eq && bin->op != BinaryOp::Ne)
    return;
  Flow &equal = bin->op == BinaryOp::Eq ? whenTrue : whenFalse;
  for (auto [side, other] : {std::pair{bin->lhs, bin->rhs}, std::pair{bin->rhs, bin->lhs}}) {
    auto *typeOf = dyn_cast<UnaryExpr>(side);
    auto *text = dyn_cast<StringLitExpr>(other);
    if (!typeOf || typeOf->op != UnaryOp::TypeOf || !text)
      continue;
    const VarSymbol *var = checkedVariable(typeOf->operand);
    if (!var || !var->type->isUnknown())
      continue;
    // A class, a box of a function and a function are "function"; a Json
    // holds no function.
    const Type *narrowed = text->value == u"string"    ? Type::string()
                           : text->value == u"number"  ? Type::doubleTy()
                           : text->value == u"boolean" ? Type::boolTy()
                           : text->value == u"object"  ? Type::anyObject()
                                                       : nullptr;
    if (narrowed)
      narrow(equal, var, narrowed);
  }
}

} // namespace tz
