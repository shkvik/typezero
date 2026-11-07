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

static Expr *afterClassValue(AstContext &ctx, Expr *object, Expr *result) {
  if (isa<NameExpr>(object)) {
    return result;
  }
  auto *seq = ctx.make<SequenceExpr>(result->loc(), std::vector<Expr *>{object, result});
  seq->type = result->type;

  return seq;
}

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

const Type *ClassValueChecker::resolveConstructorType(const TypeRef &ref) {
  if (ref.args.size() != 1) {
    this->checker.diags_.error(ref.loc,
                               "'Constructor' takes the type of the objects the class makes, e.g. "
                               "'Constructor<User>', or nothing for any class");
    return Type::error();
  }
  const Type *made = this->checker.typeResolver().resolveType(ref.args[0], false);
  if (made->isError()) {
    return made;
  }
  if ((!made->isObject() || made->isTuple()) && !made->isParam()) {
    this->checker.diags_.error(ref.args[0].loc,
                               "a class makes objects, and '" + made->name() +
                                   "' is not an object type; give the type of its objects, "
                                   "e.g. 'Constructor<User>'");
    return Type::error();
  }

  return Type::constructorOf(made);
}

const Type *ClassValueChecker::constructorArgs(llvm::SMLoc loc) {
  const ClassDecl *cls = this->checker.declaringClass_;
  if (!cls || !cls->parent) {
    this->checker.diags_.error(
        loc, "'ConstructorArgs' is the list of the parameters of the constructor of the "
             "class extended, for the constructor of a class that extends another: "
             "'constructor(...args: ConstructorArgs) { super(...args) }'");
    return Type::error();
  }
  std::vector<FunctionDecl *> ctors = ClassLinks::constructorsOf(cls->parent);
  if (ctors.empty()) {
    return Type::tuple({});
  }
  const std::string parent = cls->parent->type->name().str();
  if (ctors.size() > 1) {
    this->checker.diags_.error(
        loc, "'ConstructorArgs' is the list of the parameters of the constructor of '" + parent +
                 "', which has several; write the parameters of the one to "
                 "call");
    return Type::error();
  }
  FuncSymbol *sym = ctors[0]->sym;
  if (!sym) {
    this->checker.diags_.error(loc, "'ConstructorArgs' needs the constructor of '" + parent +
                                        "', which is declared after this class; move '" + parent +
                                        "' up");
    return Type::error();
  }
  this->checker.returnInferrer().resolveParams(sym);
  if (llvm::is_contained(sym->params, nullptr)) {
    return Type::error();
  }
  std::vector<const Type *> elements(sym->params.begin(), sym->params.end());
  const Type *rest = nullptr;
  if (sym->hasRest) {
    rest = elements.back();
    elements.pop_back();
  }

  return Type::argumentList(elements, sym->minArgs, rest);
}

Expr *ClassValueChecker::checkValueNew(NewExpr *expr, const Type *expected) {
  expr->type = Type::error();
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : expr->args) {
      arg = this->checker.exprChecker().checkExpr(arg);
    }
  };
  expr->callee = this->checker.exprChecker().checkExpr(expr->callee);
  const Type *type = expr->callee->type;
  if (type->isError()) {
    checkArgsLoosely();
    return expr;
  }
  if (!type->isClassRef()) {
    this->checker.diags_.error(expr->callee->loc(),
                               "'new' makes an object of a class, and this is a '" + type->name() +
                                   "'");
    checkArgsLoosely();
    return expr;
  }
  if (ClassDecl *cls = const_cast<ClassDecl *>(type->classRef())) {
    if (!cls->type) {
      this->checker.diags_.error(expr->callee->loc(),
                                 "'new' of '" + cls->name +
                                     "' as a value is not supported; write 'new " + cls->name +
                                     "(...)' or a literal");
      checkArgsLoosely();
      return expr;
    }
    return this->checker.methodCallChecker().checkClassNew(expr, cls, expected);
  }
  if (!expr->typeArgs.empty()) {
    this->checker.diags_.error(expr->loc(),
                               "a class known only when the program runs takes no type "
                               "arguments");
  }
  expr->form = ReflectNew{};
  std::vector<const Type *> argTypes;
  for (Expr *&arg : expr->args) {
    arg = this->checker.exprChecker().checkExpr(arg);
    argTypes.push_back(arg->type);
    if (arg->type->isVoid()) {
      this->checker.diags_.error(arg->loc(), "an expression of type 'void' has no value to pass");
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

Expr *ClassValueChecker::checkClassValue(NameExpr *name, ClassDecl *cls) {
  if (!cls->declared && this->checker.loading_ && this->checker.functions_.empty() &&
      this->checker.declModules_.lookup(cls) == this->checker.loading_) {
    this->checker.diags_.error(name->loc(),
                               "class '" + cls->name +
                                   "' is used before its declaration, which JavaScript has not run "
                                   "yet here (a ReferenceError); move the class up");
  }
  cls = ClassValueChecker::finalClass(cls);
  name->binding = NamedClass{.cls = cls};
  name->type = this->classValueType(cls);
  cls->usedAsValue = true;

  return name;
}

Expr *ClassValueChecker::checkClassRefMember(MemberExpr *member) {
  const Type *type = member->object->type;
  const ClassDecl *cls = type->classRef();
  member->type = Type::error();
  if (member->name == "prototype") {
    this->checker.diags_.error(member->loc(),
                               "there are no prototypes in tz: the methods belong to the class "
                               "itself, and a decorator gets the class; use it as it is");
    return member;
  }
  if (!cls) {
    if (member->name == "name") {
      auto *call = this->checker.ctx_.make<CallExpr>(member->loc(), member,
                                                     std::vector<Expr *>{member->object});
      call->target = BuiltinCall{.builtin = Builtin::ClassName};
      call->type = Type::string();
      return call;
    }
    this->checker.diags_.error(
        member->loc(), "'" + type->name() +
                           "' is some class known only when the program "
                           "runs, which has only its 'name'; make an object of it with 'new', "
                           "or use reflection (docs/decorators.md, section 6)");
    return member;
  }
  if (member->name == "name" &&
      (!cls->type || ClassDeclarer::methodsNamed(cls, "name", true).empty())) {
    auto *text = this->checker.ctx_.make<StringLitExpr>(
        member->loc(), std::u16string(cls->name.begin(), cls->name.end()));
    text->type = Type::string();
    return afterClassValue(this->checker.ctx_, member->object, text);
  }
  if (!cls->type) {
    this->checker.diags_.error(member->loc(),
                               "'" + cls->name + "' as a value has only its 'name' in tz");
    return member;
  }
  Expr *object = member->object;

  return afterClassValue(this->checker.ctx_, object,
                         this->checker.staticChecker().checkStaticMember(
                             member, const_cast<ClassDecl *>(cls), nullptr));
}

Expr *ClassValueChecker::classValueAt(ClassDecl *cls, llvm::SMLoc loc) {
  auto *name = this->checker.ctx_.make<NameExpr>(loc, cls->name);
  name->binding = NamedClass{.cls = cls};
  name->type = Type::classRef(cls, cls->name.empty() && cls->type ? cls->type->name() : cls->name);
  cls->usedAsValue = true;

  return name;
}

}
