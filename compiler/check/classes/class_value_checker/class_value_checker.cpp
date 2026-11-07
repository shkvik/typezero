#include "compiler/check/classes/class_value_checker/class_value_checker.h"
#include "compiler/check/classes/class_links/class_links.h"
#include "compiler/check/classes/method_call_checker/method_call_checker.h"
#include "compiler/check/classes/static_checker/static_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/check/types/type_resolver/type_resolver.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/library/reflect/reflect_checker/reflect_checker.h"
#include "llvm/ADT/STLExtras.h"

using llvm::isa;

namespace tz {

ClassValueChecker::ClassValueChecker(Checker &owner)
    : checker(owner) {}

const Type *ClassValueChecker::constructedBy(const Type *type) {
  if (type->isConstructor()) {
    return type->constructed();
  }
  const ClassDecl *cls = type->classRef();

  return cls ? cls->type : nullptr;
}

ClassDecl *ClassValueChecker::finalClass(ClassDecl *cls) {
  while (cls && cls->replacement) {
    cls = cls->replacement;
  }

  return cls;
}

const Type *ClassValueChecker::classValueType(ClassDecl *cls) {
  cls = ClassValueChecker::finalClass(cls);

  return Type::classRef(cls, cls->name.empty() && cls->type ? cls->type->name() : cls->name);
}

Expr *ClassValueChecker::afterClassValue(Expr *object, Expr *result) {
  if (isa<NameExpr>(object)) {
    return result;
  }
  auto *seq =
      this->checker.ctx_.make<SequenceExpr>(result->loc(), std::vector<Expr *>{object, result});
  seq->type = result->type;

  return seq;
}

ClassValueChecker::ResultConstructorType
ClassValueChecker::resolveConstructorType(const TypeRef &ref) {
  if (ref.args.size() != 1) {
    std::string message = ErrorTextConstructorType::typeArgs();

    return err(ClassValueCheckerResolveConstructorTypeError{
        .code = ClassValueCheckerResolveConstructorTypeErrorCode::WRONG_TYPE_ARGUMENTS,
        .message = message,
    });
  }
  const Type *made = this->checker.typeResolver().resolveType(ref.args[0], false);
  if (made->isError()) {
    return ok(made);
  }
  bool objectType = (made->isObject() && !made->isTuple()) || made->isParam();
  if (!objectType) {
    std::string message = ErrorTextConstructorType::notObject(made->name());

    return err(ClassValueCheckerResolveConstructorTypeError{
        .code = ClassValueCheckerResolveConstructorTypeErrorCode::NOT_AN_OBJECT_TYPE,
        .message = message,
    });
  }
  const Type *constructor = Type::constructorOf(made);

  return ok(constructor);
}

ClassValueChecker::ResultConstructorArgs ClassValueChecker::constructorArgs() {
  const ClassDecl *cls = this->checker.declaringClass_;
  if (!cls || !cls->parent) {
    std::string message = ErrorTextConstructorArgs::subclass();

    return err(ClassValueCheckerConstructorArgsError{
        .code = ClassValueCheckerConstructorArgsErrorCode::NOT_IN_SUBCLASS,
        .message = message,
    });
  }
  std::vector<FunctionDecl *> ctors = ClassLinks::constructorsOf(cls->parent);
  if (ctors.empty()) {
    const Type *none = Type::tuple({});

    return ok(none);
  }
  const std::string parent = cls->parent->type->name().str();
  if (ctors.size() > 1) {
    std::string message = ErrorTextConstructorArgs::several(parent);

    return err(ClassValueCheckerConstructorArgsError{
        .code = ClassValueCheckerConstructorArgsErrorCode::SEVERAL_CONSTRUCTORS,
        .message = message,
    });
  }
  FuncSymbol *sym = ctors[0]->sym;
  if (!sym) {
    std::string message = ErrorTextConstructorArgs::later(parent);

    return err(ClassValueCheckerConstructorArgsError{
        .code = ClassValueCheckerConstructorArgsErrorCode::DECLARED_LATER,
        .message = message,
    });
  }
  this->checker.returnInferrer().resolveParams(sym);
  if (llvm::is_contained(sym->params, nullptr)) {
    return ok(Type::error());
  }
  std::vector<const Type *> elements(sym->params.begin(), sym->params.end());
  const Type *rest = nullptr;
  if (sym->hasRest) {
    rest = elements.back();
    elements.pop_back();
  }
  const Type *list = Type::argumentList(elements, sym->minArgs, rest);

  return ok(list);
}

ClassValueChecker::ResultNewOfClassValue ClassValueChecker::valueNewClass(const Type *type) {
  if (!type->isClassRef()) {
    std::string message = ErrorTextValueNew::notClass(type->name());

    return err(ClassValueCheckerValueNewClassError{
        .code = ClassValueCheckerValueNewClassErrorCode::NOT_A_CLASS,
        .message = message,
    });
  }

  return ok(type);
}

ClassValueChecker::ResultClassHasItsType ClassValueChecker::typedClass(ClassDecl *cls) {
  if (!cls->type) {
    std::string message = ErrorTextValueNew::valueOnly(cls->name);

    return err(ClassValueCheckerTypedClassError{
        .code = ClassValueCheckerTypedClassErrorCode::VALUE_ONLY_CLASS,
        .message = message,
    });
  }

  return ok(cls);
}

ClassValueChecker::ResultNoTypeArguments ClassValueChecker::runtimeTypeArgs(const NewExpr *expr) {
  if (!expr->typeArgs.empty()) {
    std::string message = ErrorTextValueNew::typeArgs();

    return err(ClassValueCheckerRuntimeTypeArgsError{
        .code = ClassValueCheckerRuntimeTypeArgsErrorCode::TYPE_ARGUMENTS_GIVEN,
        .message = message,
    });
  }

  return ok(expr);
}

ClassValueChecker::ResultArgumentNotVoid ClassValueChecker::argumentValue(const Expr *arg) {
  if (arg->type->isVoid()) {
    std::string message = ErrorTextValueNew::voidArg();

    return err(ClassValueCheckerArgumentValueError{
        .code = ClassValueCheckerArgumentValueErrorCode::VOID_ARGUMENT,
        .message = message,
    });
  }

  return ok(arg);
}

Expr *ClassValueChecker::checkValueNew(NewExpr *expr, const Type *expected) {
  expr->type = Type::error();
  expr->callee = this->checker.exprChecker().checkExpr(expr->callee);
  const Type *type = expr->callee->type;
  if (type->isError()) {
    for (Expr *&arg : expr->args) {
      arg = this->checker.exprChecker().checkExpr(arg);
    }
    return expr;
  }
  ResultNewOfClassValue classValue = ClassValueChecker::valueNewClass(type);
  if (classValue.isErr()) {
    this->checker.diags_.error(expr->callee->loc(), classValue.error().message);
    for (Expr *&arg : expr->args) {
      arg = this->checker.exprChecker().checkExpr(arg);
    }
    return expr;
  }
  if (ClassDecl *cls = const_cast<ClassDecl *>(type->classRef())) {
    ResultClassHasItsType typed = ClassValueChecker::typedClass(cls);
    if (typed.isErr()) {
      this->checker.diags_.error(expr->callee->loc(), typed.error().message);
      for (Expr *&arg : expr->args) {
        arg = this->checker.exprChecker().checkExpr(arg);
      }
      return expr;
    }
    return this->checker.methodCallChecker().checkClassNew(expr, typed.value(), expected);
  }
  ResultNoTypeArguments plain = ClassValueChecker::runtimeTypeArgs(expr);
  if (plain.isErr()) {
    this->checker.diags_.error(expr->loc(), plain.error().message);
  }
  expr->form = ReflectNew{};
  std::vector<const Type *> argTypes;
  for (Expr *&arg : expr->args) {
    arg = this->checker.exprChecker().checkExpr(arg);
    argTypes.push_back(arg->type);
    ResultArgumentNotVoid value = ClassValueChecker::argumentValue(arg);
    if (value.isErr()) {
      this->checker.diags_.error(arg->loc(), value.error().message);
    } else {
      arg = this->checker.typeRelations().convert(arg, Type::unknown());
    }
  }
  if (!this->checker.reflectChecker().onlyChecked()) {
    this->checker.reflectNews_.push_back({expr, std::move(argTypes)});
  }
  this->checker.flowChecker().calledFunction();
  this->checker.reflectionUsed_ |= !this->checker.reflectChecker().onlyChecked();
  expr->type = type->constructed();

  return expr;
}

ClassValueChecker::ResultClassIsDeclared
ClassValueChecker::classDeclared(const ClassDecl *cls) const {
  bool early = !cls->declared && this->checker.loading_ && this->checker.functions_.empty() &&
               this->checker.declModules_.lookup(cls) == this->checker.loading_;
  if (early) {
    std::string message = ErrorTextClassOrder::usedEarly(cls->name);

    return err(ClassValueCheckerClassDeclaredError{
        .code = ClassValueCheckerClassDeclaredErrorCode::USED_BEFORE_DECLARATION,
        .message = message,
    });
  }

  return ok(cls);
}

Expr *ClassValueChecker::checkClassValue(NameExpr *name, ClassDecl *cls) {
  ResultClassIsDeclared declared = this->classDeclared(cls);
  if (declared.isErr()) {
    this->checker.diags_.error(name->loc(), declared.error().message);
  }
  cls = ClassValueChecker::finalClass(cls);
  name->binding = NamedClass{.cls = cls};
  name->type = this->classValueType(cls);
  cls->usedAsValue = true;

  return name;
}

ClassValueChecker::ResultClassMemberRead
ClassValueChecker::checkClassRefMember(MemberExpr *member) {
  const Type *type = member->object->type;
  const ClassDecl *cls = type->classRef();
  member->type = Type::error();
  if (member->name == "prototype") {
    std::string message = ErrorTextClassRefMember::prototype();

    return err(ClassValueCheckerCheckClassRefMemberError{
        .code = ClassValueCheckerCheckClassRefMemberErrorCode::NO_PROTOTYPES,
        .message = message,
    });
  }
  if (!cls && member->name == "name") {
    auto *call = this->checker.ctx_.make<CallExpr>(member->loc(), member,
                                                   std::vector<Expr *>{member->object});
    call->target = BuiltinCall{.builtin = Builtin::ClassName};
    call->type = Type::string();
    return ok(call);
  }
  if (!cls) {
    std::string message = ErrorTextClassRefMember::runtime(type->name());

    return err(ClassValueCheckerCheckClassRefMemberError{
        .code = ClassValueCheckerCheckClassRefMemberErrorCode::RUNTIME_CLASS,
        .message = message,
    });
  }
  bool plainName = member->name == "name" &&
                   (!cls->type || ClassDeclarer::methodsNamed(cls, "name", true).empty());
  if (plainName) {
    auto *text = this->checker.ctx_.make<StringLitExpr>(
        member->loc(), std::u16string(cls->name.begin(), cls->name.end()));
    text->type = Type::string();
    Expr *named = this->afterClassValue(member->object, text);

    return ok(named);
  }
  if (!cls->type) {
    std::string message = ErrorTextClassRefMember::nameOnly(cls->name);

    return err(ClassValueCheckerCheckClassRefMemberError{
        .code = ClassValueCheckerCheckClassRefMemberErrorCode::VALUE_ONLY_CLASS,
        .message = message,
    });
  }
  Expr *object = member->object;
  Expr *read = this->checker.staticChecker().checkStaticMember(member, const_cast<ClassDecl *>(cls),
                                                               nullptr);
  Expr *value = this->afterClassValue(object, read);

  return ok(value);
}

Expr *ClassValueChecker::classValueAt(ClassDecl *cls, llvm::SMLoc loc) {
  auto *name = this->checker.ctx_.make<NameExpr>(loc, cls->name);
  name->binding = NamedClass{.cls = cls};
  name->type = Type::classRef(cls, cls->name.empty() && cls->type ? cls->type->name() : cls->name);
  cls->usedAsValue = true;

  return name;
}

}
