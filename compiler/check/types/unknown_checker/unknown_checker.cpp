#include "compiler/check/types/unknown_checker/unknown_checker.h"
#include "compiler/check/classes/class_value_checker/class_value_checker.h"
#include "compiler/check/classes/static_checker/static_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

const Type *UnknownChecker::typeOfNarrowed(const std::u16string &name) {
  static const std::pair<const char16_t *, const Type *(*)()> kinds[] = {
      {u"string", [] { return Type::string(); }},
      {u"double", [] { return Type::doubleTy(); }},
      {u"boolean", [] { return Type::boolTy(); }},
      {u"object", [] { return Type::anyObject(); }},
      {u"class", [] { return Type::anyObject(); }},
      {u"decimal", [] { return Type::decimalTy(); }},
      {u"int", [] { return Type::intTy(); }},
      {u"int8", [] { return Type::integer(8, false); }},
      {u"int16", [] { return Type::integer(16, false); }},
      {u"int32", [] { return Type::integer(32, false); }},
      {u"uint8", [] { return Type::integer(8, true); }},
      {u"uint16", [] { return Type::integer(16, true); }},
      {u"uint32", [] { return Type::integer(32, true); }},
      {u"uint64", [] { return Type::integer(64, true); }},
      {u"array", [] { return Type::array(Type::unknown()); }},
  };
  for (auto [text, make] : kinds) {
    if (name == text) {
      return make();
    }
  }

  return nullptr;
}

const VarSymbol *UnknownChecker::checkedVariable(const Expr *expr) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit()) {
      break;
    }
    expr = conv->operand;
  }
  auto *name = dyn_cast<NameExpr>(expr);
  if (!name || !name->var() || !name->var()->type) {
    return nullptr;
  }
  const Type *type = name->var()->type;

  return type->isUnknown() || type->nonOptional()->isAnyObject() ? name->var() : nullptr;
}

std::string UnknownChecker::hintName(const Expr *expr) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit()) {
      break;
    }
    expr = conv->operand;
  }
  if (auto *name = dyn_cast<NameExpr>(expr)) {
    return name->name.str();
  }

  return "x";
}

UnknownChecker::UnknownChecker(Checker &owner)
    : checker(owner) {}

bool UnknownChecker::isObjectReference(const Type *type) {
  return type->isObject() || type->isIterator() || type->isDict() || type->isClassRef() ||
         type->isPromise() ||
         (type->isUnion() && !type->isTagged() && type->slots().front() == UnionSlot::Object);
}

bool UnknownChecker::holdsUnknown(const Type *type) {
  while (type->isOptional() || type->isArray()) {
    type = type->inner();
  }

  return type->isUnknown();
}

std::string UnknownChecker::unknownHint(const Expr *expr, const Type *to) {
  std::string x = UnknownChecker::hintName(expr);
  const Type *target = to ? to->nonOptional() : nullptr;
  bool object = expr->type && expr->type->isAnyObject();
  std::string check;
  if (target && target->isClass() && !target->classDecl()->literal()) {
    check = x + " instanceof " + target->classDecl()->name.str();
  } else if (object) {
    check = x + " instanceof C";
  } else if (target && target->isString()) {
    check = "typeof " + x + " == \"string\"";
  } else if (target && target->isNumeric()) {
    check = "typeof " + x + " == \"number\"";
  } else if (target && target->isBool()) {
    check = "typeof " + x + " == \"boolean\"";
  } else if (target && target->isArray()) {
    check = "Array.isArray(" + x + ")";
  } else if (target && target->isAnyObject()) {
    check = "typeof " + x + " == \"object\"";
  } else {
    check = "typeof " + x + " == \"string\"";
  }
  if (object) {
    return "; an object may be of any class: check which first, e.g. 'if (" + check + ") ...'";
  }

  return "; an unknown value may hold anything: check what it holds first, e.g. 'if (" + check +
         ") ...'";
}

void UnknownChecker::reportUnknownUse(const Expr *object, const std::string &what) {
  const Type *type = object->type;
  std::string x = UnknownChecker::hintName(object);
  if (type->isAnyObject()) {
    this->checker.diags_.error(
        object->loc(), "an object " + (what.empty() ? "cannot be indexed" : "has no " + what) +
                           " until 'instanceof' shows its class, e.g. 'if (" + x +
                           " instanceof C) ...'");
    return;
  }
  std::string example = what.empty() ? "Array.isArray(" + x + ")" : x + " instanceof C";
  this->checker.diags_.error(
      object->loc(), "an unknown value " + (what.empty() ? "cannot be indexed" : "has no " + what) +
                         " until a check shows what it holds, e.g. 'if (" + example +
                         ") ...' or 'typeof " + x + " == \"string\"'");
}

Expr *UnknownChecker::checkUnknownMember(MemberExpr *member) {
  member->type = Type::error();
  if (member->object->type->isClassRef()) {
    return this->checker.classValueChecker().checkClassRefMember(member);
  }
  if (member->name == "constructor") {
    this->checker.diags_.error(member->loc(),
                               "'constructor' is not supported: an object has no prototype in tz; "
                               "a decorator gets the class itself, use it as it is");
    return member;
  }
  this->reportUnknownUse(member->object, "property '" + member->name.str() + "'");

  return member;
}

Expr *UnknownChecker::checkUnknownMethod(CallExpr *call, MemberExpr *member) {
  const Type *receiver = member->object->type;
  if (!receiver->isUnknown() && !receiver->isAnyObject() && !receiver->isClassRef()) {
    return nullptr;
  }
  call->type = Type::error();
  if (ClassDecl *cls = const_cast<ClassDecl *>(receiver->classRef()); cls && cls->type) {
    Expr *object = member->object;
    Expr *result = this->checker.staticChecker().checkClassStaticCall(call, member, cls);
    if (isa<NameExpr>(object)) {
      return result;
    }
    auto *seq =
        this->checker.ctx_.make<SequenceExpr>(call->loc(), std::vector<Expr *>{object, result});
    seq->type = result->type;
    return seq;
  }
  if (receiver->isConstructor()) {
    this->checker.diags_.error(member->loc(),
                               "'" + receiver->name() +
                                   "' is some class known only when the "
                                   "program runs, whose static members are not known; call its "
                                   "methods through reflection (docs/decorators.md, section 6)");
  } else if (receiver->isClassRef()) {
    this->checker.diags_.error(member->loc(), "'" + receiver->classRef()->name +
                                                  "' as a value has no methods "
                                                  "in tz");
  } else {
    this->reportUnknownUse(member->object, "method '" + member->name.str() + "'");
  }
  for (Expr *&arg : call->args) {
    arg = this->checker.exprChecker().checkExpr(arg);
  }

  return call;
}

void UnknownChecker::addUnknownFacts(const Expr *cond, Checker::Flow &whenTrue,
                                     Checker::Flow &whenFalse) const {
  auto narrow = [](Checker::Flow &flow, const VarSymbol *var, const Type *type) {
    llvm::erase_if(flow.narrowed, [&](const auto &narrowed) { return narrowed.first == var; });
    flow.narrowed.push_back({var, type});
  };
  if (auto *call = dyn_cast<CallExpr>(cond); call && call->builtin() == Builtin::ArrayIsArray) {
    if (const VarSymbol *var = UnknownChecker::checkedVariable(call->args[0])) {
      narrow(whenTrue, var, Type::array(Type::unknown()));
    }
    return;
  }
  if (auto *call = dyn_cast<CallExpr>(cond); call && call->builtin() == Builtin::JsonIsJson) {
    if (const VarSymbol *var = UnknownChecker::checkedVariable(call->args[0])) {
      narrow(whenTrue, var, Type::json());
    }
    return;
  }
  auto *bin = dyn_cast<BinaryExpr>(cond);
  if (!bin) {
    return;
  }
  if (bin->op == BinaryOp::InstanceOf) {
    const Type *cls = bin->rhs->type;
    if (cls && cls->isConstructor()) {
      cls = cls->constructed();
    }
    if (const VarSymbol *var = UnknownChecker::checkedVariable(bin->lhs);
        var && cls && (cls->isObject() || cls->isParam() || cls->isDate() || cls->isBytes())) {
      narrow(whenTrue, var, cls);
    }
    return;
  }
  if (bin->op != BinaryOp::Eq && bin->op != BinaryOp::Ne) {
    return;
  }
  Checker::Flow &equal = bin->op == BinaryOp::Eq ? whenTrue : whenFalse;
  for (auto [side, other] : {std::pair{bin->lhs, bin->rhs}, std::pair{bin->rhs, bin->lhs}}) {
    auto *typeOf = dyn_cast<UnaryExpr>(side);
    auto *text = dyn_cast<StringLitExpr>(other);
    if (!typeOf || typeOf->op != UnaryOp::TypeOf || !text) {
      continue;
    }
    const VarSymbol *var = UnknownChecker::checkedVariable(typeOf->operand);
    if (!var || !var->type->isUnknown()) {
      continue;
    }
    if (const Type *narrowed = UnknownChecker::typeOfNarrowed(text->value)) {
      narrow(equal, var, narrowed);
    }
  }
}

}
