#include "compiler/semantic/types/unknown_checker/unknown_checker.h"
#include "compiler/program/library/object/object_member_table/object_member_table.h"
#include "compiler/program/types/type_spelling/type_spelling.h"
#include "compiler/semantic/bodies/classes/class_value_checker/class_value_checker.h"
#include "compiler/semantic/bodies/classes/static_checker/static_checker.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

UnknownChecker::UnknownChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->staticChecker = &all.staticChecker;
  this->argumentChecker = &all.argumentChecker;
  this->classValueChecker = &all.classValueChecker;
}

const VarSymbol *UnknownChecker::checkedVariable(const Expr *expr) const {
  const Expr *bare = ExprParts::withoutImplicitCasts(expr);
  auto *name = dyn_cast<NameExpr>(bare);
  if (name == nullptr || name->var() == nullptr || name->var()->type == nullptr) {
    return nullptr;
  }
  const VarSymbol *variable = name->var();
  bool isNarrowable = variable->type->isUnknown() || variable->type->nonOptional()->isAnyObject();
  if (!isNarrowable) {
    return nullptr;
  }

  return variable;
}

std::string UnknownChecker::hintName(const Expr *expr) const {
  const Expr *bare = ExprParts::withoutImplicitCasts(expr);
  auto *name = dyn_cast<NameExpr>(bare);
  if (name == nullptr) {
    return "x";
  }

  return name->name.str();
}

bool UnknownChecker::isObjectReference(const Type *type) {
  return type->isObject() || type->isIterator() || type->isDict() || type->isClassRef() ||
         type->isPromise() ||
         (type->isUnion() && !type->isTagged() && type->slots().front() == UnionSlot::Object);
}

bool UnknownChecker::isHoldingUnknown(const Type *type) const {
  while (type->isOptional() || type->isArray()) {
    type = type->inner();
  }

  return type->isUnknown();
}

std::string UnknownChecker::unknownHint(const Expr *expr, const Type *to) const {
  auto shown = this->hintName(expr);
  auto check = this->narrowingCheck(shown, expr, to);
  bool isObject = expr->type != nullptr && expr->type->isAnyObject();
  if (isObject) {
    return ErrorTextUnknownHint::anyClass(check);
  }

  return ErrorTextUnknownHint::anything(check);
}

std::string UnknownChecker::narrowingCheck(llvm::StringRef shown, const Expr *expr,
                                           const Type *to) const {
  bool isObject = expr->type != nullptr && expr->type->isAnyObject();
  if (this->isNamedClass(to)) {
    return ErrorTextUnknownHint::instanceOf(shown, to->nonOptional()->classDecl()->name);
  }
  if (isObject) {
    return ErrorTextUnknownHint::instanceOf(shown, "C");
  }
  if (to == nullptr) {
    return ErrorTextUnknownHint::typeOf(shown, "string");
  }

  return this->typeOfCheck(shown, to->nonOptional());
}

bool UnknownChecker::isNamedClass(const Type *to) const {
  if (to == nullptr || !to->nonOptional()->isClass()) {
    return false;
  }

  return to->nonOptional()->classDecl()->literal() == nullptr;
}

std::string UnknownChecker::typeOfCheck(llvm::StringRef shown, const Type *target) const {
  if (target->isString()) {
    return ErrorTextUnknownHint::typeOf(shown, "string");
  }
  if (target->isNumeric()) {
    return ErrorTextUnknownHint::typeOf(shown, "number");
  }
  if (target->isBool()) {
    return ErrorTextUnknownHint::typeOf(shown, "boolean");
  }
  if (target->isArray()) {
    return ErrorTextUnknownHint::isArray(shown);
  }
  if (target->isAnyObject()) {
    return ErrorTextUnknownHint::typeOf(shown, "object");
  }

  return ErrorTextUnknownHint::typeOf(shown, "string");
}

UnknownChecker::ResultValueNarrowed UnknownChecker::unknownUse(const Expr *object,
                                                               llvm::StringRef what) const {
  auto shown = this->hintName(object);
  if (object->type->isAnyObject()) {
    auto message = ErrorTextUnknownUse::object(shown, what);

    return err(UnknownCheckerUnknownUseError{
        .code = UnknownCheckerUnknownUseErrorCode::OBJECT_NOT_NARROWED,
        .message = message,
    });
  }
  auto message = ErrorTextUnknownUse::unknown(shown, what);

  return err(UnknownCheckerUnknownUseError{
      .code = UnknownCheckerUnknownUseErrorCode::VALUE_NOT_NARROWED,
      .message = message,
  });
}

UnknownChecker::ResultMemberChecked UnknownChecker::checkUnknownMember(MemberExpr *member) {
  member->type = Type::error();
  if (member->object->type->isClassRef()) {
    return this->checkClassMember(member);
  }
  auto constructor = this->checkNotConstructor(member);
  if (constructor.isErr()) {
    return err(constructor.error());
  }
  auto what = ErrorTextUnknownUse::property(member->name);
  auto narrowed = this->unknownUse(member->object, what);

  return err(UnknownCheckerCheckUnknownMemberError{
      .code = UnknownCheckerCheckUnknownMemberErrorCode::NOT_NARROWED,
      .message = narrowed.error().message,
  });
}

UnknownChecker::ResultMemberChecked UnknownChecker::checkClassMember(MemberExpr *member) {
  auto classMember = this->classValueChecker->checkClassRefMember(member);
  if (classMember.isErr()) {
    return err(UnknownCheckerCheckUnknownMemberError{
        .code = UnknownCheckerCheckUnknownMemberErrorCode::CLASS_MEMBER,
        .message = classMember.error().message,
    });
  }

  return ok(classMember.value());
}

UnknownChecker::ResultMemberAllowed
UnknownChecker::checkNotConstructor(const MemberExpr *member) const {
  if (member->name == ObjectMemberTable::getName(ObjectMember::CONSTRUCTOR)) {
    auto message = ErrorTextUnknownMember::noCtor();

    return err(UnknownCheckerCheckUnknownMemberError{
        .code = UnknownCheckerCheckUnknownMemberErrorCode::NO_CONSTRUCTOR,
        .message = message,
    });
  }

  return ok();
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
  auto *classDecl = const_cast<ClassDecl *>(receiver->classRef());
  if (classDecl != nullptr && classDecl->type != nullptr) {
    auto *staticCall = this->checkClassStaticCall(call, member, classDecl);

    return ok(staticCall);
  }
  this->argumentChecker->walkArgsLoosely(call);
  auto runtimeClass = this->checkNotRuntimeClass(receiver);
  if (runtimeClass.isErr()) {
    return err(runtimeClass.error());
  }
  auto classValue = this->checkNotClassValue(receiver);
  if (classValue.isErr()) {
    return err(classValue.error());
  }
  auto what = ErrorTextUnknownUse::method(member->name);
  auto narrowed = this->unknownUse(member->object, what);

  return err(UnknownCheckerCheckUnknownMethodError{
      .code = UnknownCheckerCheckUnknownMethodErrorCode::NOT_NARROWED,
      .message = narrowed.error().message,
  });
}

Expr *UnknownChecker::checkClassStaticCall(CallExpr *call, MemberExpr *member,
                                           ClassDecl *classDecl) {
  Expr *object = member->object;
  auto *result = this->staticChecker->checkClassStaticCall(call, member, classDecl);
  if (isa<NameExpr>(object)) {
    return result;
  }
  auto parts = std::vector<Expr *>{object, result};
  auto *sequence = this->state.getAst().make<SequenceExpr>(call->loc(), parts);
  sequence->type = result->type;

  return sequence;
}

UnknownChecker::ResultMethodAllowed
UnknownChecker::checkNotRuntimeClass(const Type *receiver) const {
  if (receiver->isConstructor()) {
    auto message = ErrorTextUnknownMethod::runtime(receiver->name());

    return err(UnknownCheckerCheckUnknownMethodError{
        .code = UnknownCheckerCheckUnknownMethodErrorCode::RUNTIME_CLASS,
        .message = message,
    });
  }

  return ok();
}

UnknownChecker::ResultMethodAllowed UnknownChecker::checkNotClassValue(const Type *receiver) const {
  if (receiver->isClassRef()) {
    auto message = ErrorTextUnknownMethod::classValue(receiver->classRef()->name);

    return err(UnknownCheckerCheckUnknownMethodError{
        .code = UnknownCheckerCheckUnknownMethodErrorCode::CLASS_VALUE,
        .message = message,
    });
  }

  return ok();
}

void UnknownChecker::addUnknownFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const {
  auto *call = dyn_cast<CallExpr>(cond);
  auto *binary = dyn_cast<BinaryExpr>(cond);

  if (call != nullptr && call->builtin() == Builtin::ArrayIsArray) {
    const auto *unknownArray = Type::array(Type::unknown());

    return this->narrowChecked(call->args[0], unknownArray, whenTrue);
  }
  if (call != nullptr && call->builtin() == Builtin::JsonIsJson) {
    return this->narrowChecked(call->args[0], Type::json(), whenTrue);
  }
  if (binary != nullptr && binary->op == BinaryOp::InstanceOf) {
    return this->narrowByInstanceOf(binary, whenTrue);
  }
  if (binary != nullptr && binary->op == BinaryOp::Eq) {
    return this->narrowByTypeOf(binary, whenTrue);
  }
  if (binary != nullptr && binary->op == BinaryOp::Ne) {
    return this->narrowByTypeOf(binary, whenFalse);
  }
}

void UnknownChecker::narrowChecked(const Expr *checked, const Type *type, Flow &flow) const {
  const auto *variable = this->checkedVariable(checked);
  if (variable != nullptr) {
    this->replaceNarrowing(flow, variable, type);
  }
}

void UnknownChecker::narrowByInstanceOf(const BinaryExpr *binary, Flow &whenTrue) const {
  const Type *instance = this->instanceType(binary->rhs->type);
  bool isNarrowing = instance != nullptr && (instance->isObject() || instance->isParam() ||
                                             instance->isDate() || instance->isBytes());
  if (isNarrowing) {
    this->narrowChecked(binary->lhs, instance, whenTrue);
  }
}

const Type *UnknownChecker::instanceType(const Type *classValue) const {
  if (classValue != nullptr && classValue->isConstructor()) {
    return classValue->constructed();
  }

  return classValue;
}

void UnknownChecker::narrowByTypeOf(const BinaryExpr *binary, Flow &equal) const {
  this->narrowTypeOfSide(binary->lhs, binary->rhs, equal);
  this->narrowTypeOfSide(binary->rhs, binary->lhs, equal);
}

void UnknownChecker::narrowTypeOfSide(const Expr *side, const Expr *other, Flow &equal) const {
  auto *typeOf = dyn_cast<UnaryExpr>(side);
  auto *text = dyn_cast<StringLitExpr>(other);
  bool isTypeOfTest = typeOf != nullptr && typeOf->op == UnaryOp::TypeOf && text != nullptr;
  if (!isTypeOfTest) {
    return;
  }
  const auto *variable = this->checkedVariable(typeOf->operand);
  if (variable == nullptr || !variable->type->isUnknown()) {
    return;
  }
  const auto *narrowed = TypeSpelling::narrowedByTypeOf(text->value);
  if (narrowed != nullptr) {
    this->replaceNarrowing(equal, variable, narrowed);
  }
}

void UnknownChecker::replaceNarrowing(Flow &flow, const VarSymbol *variable,
                                      const Type *type) const {
  auto kept = std::vector<std::pair<const VarSymbol *, const Type *>>();
  for (const auto &narrowing : flow.narrowed) {
    if (narrowing.first != variable) {
      kept.push_back(narrowing);
    }
  }
  kept.push_back({variable, type});
  flow.narrowed = kept;
}

}
