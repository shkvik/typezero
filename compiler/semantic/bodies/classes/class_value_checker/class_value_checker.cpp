#include "compiler/semantic/bodies/classes/class_value_checker/class_value_checker.h"
#include "compiler/semantic/bodies/classes/class_links/class_links.h"
#include "compiler/semantic/bodies/classes/method_call_checker/method_call_checker.h"
#include "compiler/semantic/bodies/classes/static_checker/static_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/library/reflect/reflect_checker/reflect_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "llvm/ADT/STLExtras.h"

using llvm::isa;

namespace tz {

ClassValueChecker::ClassValueChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->classLinks = &all.classLinks;
  this->exprChecker = &all.exprChecker;
  this->flowChecker = &all.flowChecker;
  this->typeResolver = &all.typeResolver;
  this->staticChecker = &all.staticChecker;
  this->typeRelations = &all.typeRelations;
  this->classDeclarer = &all.classDeclarer;
  this->reflectChecker = &all.reflectChecker;
  this->methodCallChecker = &all.methodCallChecker;
}

const Type *ClassValueChecker::constructedBy(const Type *type) {
  if (type->isConstructor()) {
    return type->constructed();
  }
  const ClassDecl *classDecl = type->classRef();

  return classDecl != nullptr ? classDecl->type : nullptr;
}

ClassDecl *ClassValueChecker::finalClass(ClassDecl *classDecl) const {
  while (classDecl != nullptr && classDecl->replacement != nullptr) {
    classDecl = classDecl->replacement;
  }

  return classDecl;
}

const Type *ClassValueChecker::classValueType(ClassDecl *classDecl) {
  classDecl = this->finalClass(classDecl);

  return Type::classRef(classDecl, classDecl->name.empty() && classDecl->type != nullptr
                                       ? classDecl->type->name()
                                       : classDecl->name);
}

Expr *ClassValueChecker::afterClassValue(Expr *object, Expr *result) {
  if (isa<NameExpr>(object)) {
    return result;
  }
  auto *sequence =
      this->state.getAst().make<SequenceExpr>(result->loc(), std::vector<Expr *>{object, result});
  sequence->type = result->type;

  return sequence;
}

ClassValueChecker::ResultConstructorType
ClassValueChecker::resolveConstructorType(const TypeRef &ref) {
  if (ref.args.size() != 1) {
    auto message = ErrorTextConstructorType::typeArgs();

    return err(ClassValueCheckerResolveConstructorTypeError{
        .code = ClassValueCheckerResolveConstructorTypeErrorCode::WRONG_TYPE_ARGUMENTS,
        .message = message,
    });
  }
  const auto *made = this->typeResolver->resolveType(ref.args[0]);
  if (made->isError()) {
    return ok(made);
  }
  bool isObjectType = (made->isObject() && !made->isTuple()) || made->isParam();
  if (!isObjectType) {
    auto message = ErrorTextConstructorType::notObject(made->name());

    return err(ClassValueCheckerResolveConstructorTypeError{
        .code = ClassValueCheckerResolveConstructorTypeErrorCode::NOT_AN_OBJECT_TYPE,
        .message = message,
    });
  }
  const auto *constructor = Type::constructorOf(made);

  return ok(constructor);
}

ClassValueChecker::ResultConstructorArgs ClassValueChecker::constructorArgs() {
  const auto *classDecl = this->state.getLateClassStore().getDeclaringClass();
  if (classDecl == nullptr || classDecl->parent == nullptr) {
    auto message = ErrorTextConstructorArgs::subclass();

    return err(ClassValueCheckerConstructorArgsError{
        .code = ClassValueCheckerConstructorArgsErrorCode::NOT_IN_SUBCLASS,
        .message = message,
    });
  }
  auto constructors = this->classLinks->constructorsOf(classDecl->parent);
  if (constructors.empty()) {
    const auto *none = Type::tuple({});

    return ok(none);
  }
  const std::string parent = classDecl->parent->type->name().str();
  if (constructors.size() > 1) {
    auto message = ErrorTextConstructorArgs::several(parent);

    return err(ClassValueCheckerConstructorArgsError{
        .code = ClassValueCheckerConstructorArgsErrorCode::SEVERAL_CONSTRUCTORS,
        .message = message,
    });
  }
  FuncSymbol *function = constructors[0]->sym;
  if (function == nullptr) {
    auto message = ErrorTextConstructorArgs::later(parent);

    return err(ClassValueCheckerConstructorArgsError{
        .code = ClassValueCheckerConstructorArgsErrorCode::DECLARED_LATER,
        .message = message,
    });
  }
  if (llvm::is_contained(function->params, nullptr)) {
    return ok(Type::error());
  }
  std::vector<const Type *> elements(function->params.begin(), function->params.end());
  const Type *rest = nullptr;
  if (function->hasRest) {
    rest = elements.back();
    elements.pop_back();
  }
  const auto *list = Type::argumentList(elements, function->minArgs, rest);

  return ok(list);
}

ClassValueChecker::ResultNewOfClassValue ClassValueChecker::valueNewClass(const Type *type) const {
  if (!type->isClassRef()) {
    auto message = ErrorTextValueNew::notClass(type->name());

    return err(ClassValueCheckerValueNewClassError{
        .code = ClassValueCheckerValueNewClassErrorCode::NOT_A_CLASS,
        .message = message,
    });
  }

  return ok(type);
}

ClassValueChecker::ResultClassHasItsType ClassValueChecker::typedClass(ClassDecl *classDecl) const {
  if (classDecl->type == nullptr) {
    auto message = ErrorTextValueNew::valueOnly(classDecl->name);

    return err(ClassValueCheckerTypedClassError{
        .code = ClassValueCheckerTypedClassErrorCode::VALUE_ONLY_CLASS,
        .message = message,
    });
  }

  return ok(classDecl);
}

ClassValueChecker::ResultNoTypeArguments
ClassValueChecker::runtimeTypeArgs(const NewExpr *expr) const {
  if (!expr->typeArgs.empty()) {
    auto message = ErrorTextValueNew::typeArgs();

    return err(ClassValueCheckerRuntimeTypeArgsError{
        .code = ClassValueCheckerRuntimeTypeArgsErrorCode::TYPE_ARGUMENTS_GIVEN,
        .message = message,
    });
  }

  return ok(expr);
}

ClassValueChecker::ResultArgumentNotVoid ClassValueChecker::argumentValue(const Expr *arg) const {
  if (arg->type->isVoid()) {
    auto message = ErrorTextValueNew::voidArg();

    return err(ClassValueCheckerArgumentValueError{
        .code = ClassValueCheckerArgumentValueErrorCode::VOID_ARGUMENT,
        .message = message,
    });
  }

  return ok(arg);
}

Expr *ClassValueChecker::checkValueNew(NewExpr *expr, const Type *expected) {
  expr->type = Type::error();
  expr->callee = this->exprChecker->checkExpr(expr->callee);
  const Type *type = expr->callee->type;
  if (type->isError()) {
    for (Expr *&arg : expr->args) {
      arg = this->exprChecker->checkExpr(arg);
    }
    return expr;
  }
  auto classValue = this->valueNewClass(type);
  if (classValue.isErr()) {
    this->state.getDiagnostics().error(expr->callee->loc(), classValue.error().message);
    for (Expr *&arg : expr->args) {
      arg = this->exprChecker->checkExpr(arg);
    }
    return expr;
  }
  auto *classDecl = const_cast<ClassDecl *>(type->classRef());
  if (classDecl != nullptr) {
    auto typed = this->typedClass(classDecl);
    if (typed.isErr()) {
      this->state.getDiagnostics().error(expr->callee->loc(), typed.error().message);
      for (Expr *&arg : expr->args) {
        arg = this->exprChecker->checkExpr(arg);
      }
      return expr;
    }
    return this->methodCallChecker->checkClassNew(expr, typed.value(), expected);
  }
  auto plain = this->runtimeTypeArgs(expr);
  if (plain.isErr()) {
    this->state.getDiagnostics().error(expr->loc(), plain.error().message);
  }
  expr->form = ReflectNew{};
  auto argTypes = std::vector<const Type *>();
  for (Expr *&arg : expr->args) {
    arg = this->exprChecker->checkExpr(arg);
    argTypes.push_back(arg->type);
    auto value = this->argumentValue(arg);
    if (value.isErr()) {
      this->state.getDiagnostics().error(arg->loc(), value.error().message);
    } else {
      arg = this->typeRelations->convert(arg, Type::unknown());
    }
  }
  if (!this->reflectChecker->onlyChecked()) {
    this->state.getReflectStore().addReflectedNew(expr, std::move(argTypes));
  }
  this->flowChecker->calledFunction();
  if (!this->reflectChecker->onlyChecked()) {
    this->state.getReflectStore().markReflectionUsed();
  }
  expr->type = type->constructed();

  return expr;
}

ClassValueChecker::ResultClassIsDeclared
ClassValueChecker::classDeclared(const ClassDecl *classDecl) const {
  bool isEarly = !classDecl->declared &&
                 this->state.getModuleStates().getLoadingModule() != nullptr &&
                 !this->state.getFunctionStack().isInsideFunction() &&
                 this->state.getModuleStates().findDeclModule(classDecl) ==
                     this->state.getModuleStates().getLoadingModule();
  if (isEarly) {
    auto message = ErrorTextClassOrder::usedEarly(classDecl->name);

    return err(ClassValueCheckerClassDeclaredError{
        .code = ClassValueCheckerClassDeclaredErrorCode::USED_BEFORE_DECLARATION,
        .message = message,
    });
  }

  return ok(classDecl);
}

Expr *ClassValueChecker::checkClassValue(NameExpr *name, ClassDecl *classDecl) {
  auto declared = this->classDeclared(classDecl);
  if (declared.isErr()) {
    this->state.getDiagnostics().error(name->loc(), declared.error().message);
  }
  classDecl = this->finalClass(classDecl);
  name->binding = NamedClass{.cls = classDecl};
  name->type = this->classValueType(classDecl);
  classDecl->usedAsValue = true;

  return name;
}

ClassValueChecker::ResultClassMemberRead
ClassValueChecker::checkClassRefMember(MemberExpr *member) {
  const Type *type = member->object->type;
  const ClassDecl *classDecl = type->classRef();
  member->type = Type::error();
  if (member->name == "prototype") {
    auto message = ErrorTextClassRefMember::prototype();

    return err(ClassValueCheckerCheckClassRefMemberError{
        .code = ClassValueCheckerCheckClassRefMemberErrorCode::NO_PROTOTYPES,
        .message = message,
    });
  }
  if (classDecl == nullptr && member->name == "name") {
    auto *call = this->state.getAst().make<CallExpr>(member->loc(), member,
                                                     std::vector<Expr *>{member->object});
    call->target = BuiltinCall{.builtin = Builtin::ClassName};
    call->type = Type::string();
    return ok(call);
  }
  if (classDecl == nullptr) {
    auto message = ErrorTextClassRefMember::runtime(type->name());

    return err(ClassValueCheckerCheckClassRefMemberError{
        .code = ClassValueCheckerCheckClassRefMemberErrorCode::RUNTIME_CLASS,
        .message = message,
    });
  }
  bool isPlainName = member->name == "name" &&
                     (classDecl->type == nullptr ||
                      this->classDeclarer->staticMethodsNamed(classDecl, "name").empty());
  if (isPlainName) {
    auto *text = this->state.getAst().make<StringLitExpr>(
        member->loc(), std::u16string(classDecl->name.begin(), classDecl->name.end()));
    text->type = Type::string();
    auto *named = this->afterClassValue(member->object, text);

    return ok(named);
  }
  if (classDecl->type == nullptr) {
    auto message = ErrorTextClassRefMember::nameOnly(classDecl->name);

    return err(ClassValueCheckerCheckClassRefMemberError{
        .code = ClassValueCheckerCheckClassRefMemberErrorCode::VALUE_ONLY_CLASS,
        .message = message,
    });
  }
  Expr *object = member->object;
  auto *read =
      this->staticChecker->checkStaticMember(member, const_cast<ClassDecl *>(classDecl), nullptr);
  auto *value = this->afterClassValue(object, read);

  return ok(value);
}

Expr *ClassValueChecker::classValueAt(ClassDecl *classDecl, llvm::SMLoc loc) {
  auto *name = this->state.getAst().make<NameExpr>(loc, classDecl->name);
  name->binding = NamedClass{.cls = classDecl};
  name->type = Type::classRef(classDecl, classDecl->name.empty() && classDecl->type != nullptr
                                             ? classDecl->type->name()
                                             : classDecl->name);
  classDecl->usedAsValue = true;

  return name;
}

}
