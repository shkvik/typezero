#include "compiler/semantic/types/unknown_checker/unknown_checker.h"
#include "compiler/semantic/bodies/classes/class_value_checker/class_value_checker.h"
#include "compiler/semantic/bodies/classes/static_checker/static_checker.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
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

UnknownChecker::UnknownChecker(SemanticState &owner)
    : state(owner) {}

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
    check = ErrorTextUnknownHint::instanceOf(x, target->classDecl()->name);
  } else if (object) {
    check = ErrorTextUnknownHint::instanceOf(x, "C");
  } else if (target && target->isString()) {
    check = ErrorTextUnknownHint::typeOf(x, "string");
  } else if (target && target->isNumeric()) {
    check = ErrorTextUnknownHint::typeOf(x, "number");
  } else if (target && target->isBool()) {
    check = ErrorTextUnknownHint::typeOf(x, "boolean");
  } else if (target && target->isArray()) {
    check = ErrorTextUnknownHint::isArray(x);
  } else if (target && target->isAnyObject()) {
    check = ErrorTextUnknownHint::typeOf(x, "object");
  } else {
    check = ErrorTextUnknownHint::typeOf(x, "string");
  }
  if (object) {
    return ErrorTextUnknownHint::anyClass(check);
  }

  return ErrorTextUnknownHint::anything(check);
}

UnknownChecker::ResultValueNarrowed UnknownChecker::unknownUse(const Expr *object,
                                                               llvm::StringRef what) {
  std::string x = UnknownChecker::hintName(object);
  if (object->type->isAnyObject()) {
    std::string message = ErrorTextUnknownUse::object(x, what);

    return err(UnknownCheckerUnknownUseError{
        .code = UnknownCheckerUnknownUseErrorCode::OBJECT_NOT_NARROWED,
        .message = message,
    });
  }

  std::string message = ErrorTextUnknownUse::unknown(x, what);

  return err(UnknownCheckerUnknownUseError{
      .code = UnknownCheckerUnknownUseErrorCode::VALUE_NOT_NARROWED,
      .message = message,
  });
}

UnknownChecker::ResultMemberChecked UnknownChecker::checkUnknownMember(MemberExpr *member) {
  member->type = Type::error();
  if (member->object->type->isClassRef()) {
    ClassValueChecker::ResultClassMemberRead classMember =
        ClassValueChecker(this->state).checkClassRefMember(member);
    if (classMember.isErr()) {
      return err(UnknownCheckerCheckUnknownMemberError{
          .code = UnknownCheckerCheckUnknownMemberErrorCode::CLASS_MEMBER,
          .message = classMember.error().message,
      });
    }
    return ok(classMember.value());
  }
  if (member->name == "constructor") {
    std::string message = ErrorTextUnknownMember::noCtor();

    return err(UnknownCheckerCheckUnknownMemberError{
        .code = UnknownCheckerCheckUnknownMemberErrorCode::NO_CONSTRUCTOR,
        .message = message,
    });
  }

  std::string what = ErrorTextUnknownUse::property(member->name);
  ResultValueNarrowed narrowed = UnknownChecker::unknownUse(member->object, what);

  return err(UnknownCheckerCheckUnknownMemberError{
      .code = UnknownCheckerCheckUnknownMemberErrorCode::NOT_NARROWED,
      .message = narrowed.error().message,
  });
}

UnknownChecker::ResultMethodChecked UnknownChecker::checkUnknownMethod(CallExpr *call,
                                                                       MemberExpr *member) {
  const Type *receiver = member->object->type;
  if (!receiver->isUnknown() && !receiver->isAnyObject() && !receiver->isClassRef()) {
    return err(UnknownCheckerCheckUnknownMethodError{
        .code = UnknownCheckerCheckUnknownMethodErrorCode::NOT_UNKNOWN,
        .message = "",
    });
  }
  call->type = Type::error();
  if (ClassDecl *cls = const_cast<ClassDecl *>(receiver->classRef()); cls && cls->type) {
    Expr *object = member->object;
    Expr *result = StaticChecker(this->state).checkClassStaticCall(call, member, cls);
    if (isa<NameExpr>(object)) {
      return ok(result);
    }
    auto *seq =
        this->state.getAst().make<SequenceExpr>(call->loc(), std::vector<Expr *>{object, result});
    seq->type = result->type;
    return ok(seq);
  }
  ArgumentChecker(this->state).checkArgsLoosely(call);
  if (receiver->isConstructor()) {
    std::string message = ErrorTextUnknownMethod::runtime(receiver->name());

    return err(UnknownCheckerCheckUnknownMethodError{
        .code = UnknownCheckerCheckUnknownMethodErrorCode::RUNTIME_CLASS,
        .message = message,
    });
  }
  if (receiver->isClassRef()) {
    std::string message = ErrorTextUnknownMethod::classValue(receiver->classRef()->name);

    return err(UnknownCheckerCheckUnknownMethodError{
        .code = UnknownCheckerCheckUnknownMethodErrorCode::CLASS_VALUE,
        .message = message,
    });
  }

  std::string what = ErrorTextUnknownUse::method(member->name);
  ResultValueNarrowed narrowed = UnknownChecker::unknownUse(member->object, what);

  return err(UnknownCheckerCheckUnknownMethodError{
      .code = UnknownCheckerCheckUnknownMethodErrorCode::NOT_NARROWED,
      .message = narrowed.error().message,
  });
}

void UnknownChecker::addUnknownFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const {
  auto narrow = [](Flow &flow, const VarSymbol *var, const Type *type) {
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
  Flow &equal = bin->op == BinaryOp::Eq ? whenTrue : whenFalse;
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
