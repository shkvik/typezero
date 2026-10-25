// Reflection (docs/decorators.md, section 6): Reflect.get, Reflect.apply and
// Reflect.construct, and fromJson(cls, j), which makes an object of a class
// from a Json through its constructor (docs/narrowing.md, section 3).
//
// The values reflection gives and takes are unknowns, checked against the
// parameters when the program runs; a mismatch is a TypeError. Each class it
// reaches is described in the exe (see codegen_reflect.cpp), and no other:
// one that becomes a `Constructor<T>` gets the function that makes its
// objects, and, when the program looks into objects by name, the classes
// whose objects may get there (as an `object`, an `unknown`, or made by
// reflection) get their methods. Where the class and the name are known when
// compiling, the call is what it stands for: `Reflect.construct(User, [a])`
// is `new User(a)`, `Reflect.get(user, "name")` reads the field.

#include "sema/sema.h"

#include "llvm/ADT/STLExtras.h"

#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

// The classes whose objects a value of `type` may be.
void classesIn(const Type *type, llvm::SmallPtrSetImpl<ClassDecl *> &out) {
  type = type->nonOptional();
  if (type->isArray()) {
    classesIn(type->inner(), out);
    return;
  }
  if (type->isUnion()) {
    for (const Type *m : type->members())
      classesIn(m, out);
    return;
  }
  if (ClassDecl *cls = const_cast<ClassDecl *>(type->classDecl()); cls && !cls->literal)
    out.insert(cls);
}

} // namespace

void Sema::noteReflectedConversion(const Type *from, const Type *to) {
  // A class that becomes some class known only when the program runs, which
  // `new` and Reflect.construct make objects of.
  if (to->nonOptional()->isConstructor()) {
    if (ClassDecl *cls = const_cast<ClassDecl *>(from->nonOptional()->classRef()))
      cls->reflectNew = true;
    return;
  }
  // Objects that code looks into only through reflection.
  if (to->nonOptional()->isAnyObject() || to->isUnknown() ||
      (to->nonOptional()->isArray() && to->nonOptional()->inner()->isUnknown()))
    classesIn(from, escaped_);
}

void Sema::finishReflection() {
  if (!reflectionUsed_) {
    // Nothing looks into a class by name: none is described.
    for (ClassDecl *cls : classes_)
      cls->reflectNew = cls->reflectMethods = cls->reflectJson = false;
    return;
  }
  // The objects reflection may look into: of the classes they escape as, of
  // those it makes, and of the classes down their chains.
  std::function<void(ClassDecl *)> methods = [&](ClassDecl *cls) {
    if (cls->reflectMethods)
      return;
    cls->reflectMethods = true;
    for (ClassDecl *child : cls->children)
      methods(child);
  };
  if (reflectGetAny_) {
    for (ClassDecl *cls : escaped_)
      methods(cls);
    for (ClassDecl *cls : classes_)
      if (cls->reflectNew)
        methods(cls);
  }
  for (ClassDecl *cls : reflectedObjects_)
    methods(cls);
  // fromJson(cls, j) of a class known only when the program runs: any class
  // that may be one, if a Json can make it.
  if (jsonReflect_)
    for (ClassDecl *cls : classes_)
      if (cls->reflectNew && !cls->isAbstract && classJsonProblem(cls).empty())
        cls->reflectJson = true;
}

std::string Sema::classJsonProblem(const ClassDecl *cls) {
  auto *c = const_cast<ClassDecl *>(cls);
  if (auto found = jsonProblems_.find(cls); found != jsonProblems_.end())
    return found->second;
  jsonProblems_[cls] = ""; // a class that holds itself, as it is being found
  std::string name = cls->type ? cls->type->name().str() : cls->name.str();
  std::string problem;
  std::vector<FunctionDecl *> ctors = constructorsOf(cls);
  if (cls->builtin || isRuntimeClass(cls))
    problem = "'" + name + "' is made only by the runtime";
  else if (cls->isAbstract)
    problem = "'" + name + "' is abstract, so no object is only a '" + name + "'";
  else if (ctors.size() > 1)
    problem = "'" + name + "' has several constructors, and fromJson calls the one a class has";
  const FunctionDecl *ctor = ctors.empty() ? nullptr : ctors.front();
  c->jsonParams.clear();
  if (problem.empty() && ctor && ctor->sym) {
    resolveParams(ctor->sym);
    // The key of each parameter is its name; those of `...args:
    // ConstructorArgs` have the names of the parameters they are up the
    // chain.
    std::function<std::string(const FunctionDecl *, size_t)> keyOf = [&](const FunctionDecl *fn,
                                                                          size_t k) {
      if (!fn->listName.empty() && k >= fn->listStart && fn->cls && fn->cls->parent) {
        std::vector<FunctionDecl *> up = constructorsOf(fn->cls->parent);
        if (up.size() == 1 && k - fn->listStart < up[0]->params.size())
          return keyOf(up[0], k - fn->listStart);
      }
      return fn->params[k].name.str();
    };
    for (size_t i = 0; i < ctor->sym->params.size() && i < ctor->params.size(); ++i)
      c->jsonParams.push_back(keyOf(ctor, i));
    for (size_t i = 0; i < ctor->sym->params.size() && problem.empty(); ++i) {
      const Type *type = ctor->sym->params[i];
      if (!type || (ctor->sym->hasRest && i + 1 == ctor->sym->params.size())) {
        problem = "its constructor takes a rest parameter, which no key of a Json names";
        break;
      }
      std::string why =
          jsonProblem(type, [&](const ClassDecl *inner) { return classJsonProblem(inner); });
      if (!why.empty())
        problem = "parameter '" + c->jsonParams[i] + "' of its constructor: " + why;
    }
  }
  if (problem.empty()) {
    // The fields code may write that a Json can give, which the keys of the
    // Json set after the constructor ran.
    completeClass(c);
    c->jsonFields.clear();
    for (const Field &f : c->type->fields()) {
      if (llvm::is_contained(c->jsonParams, f.name))
        continue;
      const ClassDecl *owner = nullptr;
      const ClassField *field = findField(cls, f.name, &owner);
      if (!field || field->isStatic || field->method || f.readonly ||
          field->access != Access::Public || f.name[0] == '#')
        continue;
      if (jsonProblem(f.type, [&](const ClassDecl *inner) { return classJsonProblem(inner); })
              .empty())
        c->jsonFields.push_back(f.name);
    }
  }
  jsonProblems_[cls] = problem;
  return problem;
}

// An array of the arguments of Reflect.apply or Reflect.construct: of any
// values, each an unknown when the program runs; a literal is one of
// unknowns.
Expr *Sema::checkReflectArgs(Expr *args) {
  if (isa<ArrayLitExpr>(args))
    return checkExpr(args, Type::array(Type::unknown()));
  args = checkExpr(args);
  const Type *type = args->type;
  if (type->isError() || type->isArray())
    return args;
  if (type->isTuple())
    diags_.error(args->loc(), "the arguments are an array, and this is a tuple; copy it into one, "
                              "'[...t]'");
  else
    diags_.error(args->loc(), "the arguments are an array, e.g. '[a, b]', found '" + type->name() +
                                  "'");
  args->type = Type::error();
  return args;
}

Expr *Sema::checkReflection(CallExpr *call, MemberExpr *member, const Type *expected) {
  call->type = Type::error();
  llvm::StringRef fn = member->name;
  auto checkArgsLoosely = [&] {
    for (Expr *&arg : call->args)
      arg = checkExpr(arg);
  };
  if (rejectSpread(call))
    return call;
  if (fn == "get") {
    if (!checkArity(call, member->loc(), 2, 2))
      return call;
    // A field known when compiling: read as it is.
    auto *object = dyn_cast<NameExpr>(call->args[0]);
    auto *key = dyn_cast<StringLitExpr>(call->args[1]);
    Expr *&target = call->args[0];
    target = checkExpr(target);
    const Type *type = target->type;
    if (type->isError()) {
      call->args[1] = checkExpr(call->args[1]);
      return call;
    }
    const Type *base = type->nonOptional();
    if (base->isJson()) {
      diags_.error(target->loc(), "Reflect.get takes an object of the language; read a key of a "
                                  "Json as JavaScript does, 'j[key]'");
      checkArgsLoosely();
      return call;
    }
    if (!type->isUnknown() && !isObjectReference(base) && !base->isAnyObject()) {
      diags_.error(target->loc(), "Reflect.get takes an object, and this is a '" + type->name() +
                                      "'");
      call->args[1] = checkExpr(call->args[1]);
      return call;
    }
    if (object && key && !type->isOptional() && base->isObject() && !base->isAnyObject() &&
        llvm::all_of(key->value, [](char16_t c) { return c < 128; })) {
      std::string name;
      for (char16_t c : key->value)
        name.push_back(char(c));
      ClassDecl *cls = classOf(base);
      if (cls)
        completeClass(cls);
      const ClassField *declared = cls ? findField(cls, name) : nullptr;
      if (base->field(name) && (!cls || (declared && declared->access == Access::Public))) {
        auto *read = ctx_.make<MemberExpr>(
            key->loc(), ctx_.make<NameExpr>(object->loc(), object->name), ctx_.keepName(name));
        return convert(checkExpr(read), Type::unknown());
      }
    }
    call->args[1] = checkArg(call->args[1], Type::string());
    // Objects of these classes are looked into by name.
    if (type->isUnknown() || base->isAnyObject() || !base->classDecl())
      reflectGetAny_ = true;
    else
      classesIn(base, reflectedObjects_);
    target = convert(target, Type::unknown());
    reflectionUsed_ = true;
    call->builtin = Builtin::ReflectGet;
    call->type = Type::unknown();
    return call;
  }
  if (fn == "apply") {
    if (!checkArity(call, member->loc(), 3, 3))
      return call;
    Expr *&f = call->args[0];
    f = checkExpr(f);
    if (!f->type->isError() && !f->type->isUnknown() && !f->type->isFunction())
      diags_.error(f->loc(), "Reflect.apply calls a function, and this is a '" + f->type->name() +
                                 "'");
    else
      f = convert(f, Type::unknown());
    Expr *&self = call->args[1];
    self = checkExpr(self);
    if (self->type->isVoid())
      diags_.error(self->loc(), "an expression of type 'void' has no value to pass as 'this'");
    else
      self = convert(self, Type::unknown());
    call->args[2] = checkReflectArgs(call->args[2]);
    calledFunction();
    reflectionUsed_ = true;
    reflectGetAny_ = true; // the method of an object it got by name
    call->builtin = Builtin::ReflectApply;
    call->type = Type::unknown();
    return call;
  }
  // Reflect.construct(cls, args)
  if (call->args.size() == 3) {
    diags_.error(call->args[2]->loc(), "the third argument of Reflect.construct, the class whose "
                                       "prototype the object gets, is not supported: there are no "
                                       "prototypes in tz; make an object of that class");
    checkArgsLoosely();
    return call;
  }
  if (!checkArity(call, member->loc(), 2, 2))
    return call;
  // A class known when compiling, and the arguments written out: `new`.
  auto *name = dyn_cast<NameExpr>(call->args[0]);
  auto *list = dyn_cast<ArrayLitExpr>(call->args[1]);
  Expr *&cls = call->args[0];
  cls = checkExpr(cls);
  const Type *type = cls->type;
  if (type->isError()) {
    call->args[1] = checkReflectArgs(call->args[1]);
    return call;
  }
  if (!type->isClassRef()) {
    diags_.error(cls->loc(), "Reflect.construct makes an object of a class, and this is a '" +
                                 type->name() + "'");
    call->args[1] = checkReflectArgs(call->args[1]);
    return call;
  }
  if (name && list && type->classRef() &&
      llvm::none_of(list->elements, [](const ArrayLitExpr::Element &e) { return e.spread; })) {
    std::vector<Expr *> args;
    for (const ArrayLitExpr::Element &e : list->elements)
      args.push_back(e.expr);
    auto *create = ctx_.make<NewExpr>(call->loc(), "", std::vector<TypeRef>(), std::move(args));
    create->callee = ctx_.make<NameExpr>(name->loc(), name->name);
    return checkValueNew(create, expected);
  }
  if (const ClassDecl *known = type->classRef()) {
    if (!known->type) {
      diags_.error(cls->loc(), "'" + known->name + "' of JavaScript cannot be made through "
                                                   "reflection");
      call->args[1] = checkReflectArgs(call->args[1]);
      return call;
    }
    cls = convert(cls, Type::constructorOf(known->type));
  }
  call->args[1] = checkReflectArgs(call->args[1]);
  calledFunction();
  reflectionUsed_ = true;
  call->builtin = Builtin::ReflectConstruct;
  call->type = constructedBy(cls->type);
  return call;
}

Expr *Sema::checkFromJsonClass(CallExpr *call, NameExpr *name) {
  call->type = Type::error();
  Expr *&cls = call->args[0];
  cls = checkExpr(cls);
  Expr *arg = checkExpr(call->args[1], Type::json());
  call->args[1] = coerce(arg, Type::json());
  const Type *type = cls->type;
  if (type->isError())
    return call;
  if (!type->isClassRef()) {
    diags_.error(cls->loc(),
                 "fromJson(cls, j) makes an object of the class 'cls', and this is a '" +
                     type->name() + "'; to build a value of a type, write 'fromJson<" +
                     type->name() + ">(j)'");
    return call;
  }
  if (const ClassDecl *known = type->classRef()) {
    std::string problem = known->type ? classJsonProblem(known)
                                      : "'" + known->name.str() + "' is a class of JavaScript";
    if (!problem.empty()) {
      diags_.error(name->loc(), "fromJson cannot make a '" + known->name.str() +
                                    "' from a Json: " + problem);
      return call;
    }
    call->type = known->type;
  } else {
    // Any class it may be, through its description.
    reflectionUsed_ = true;
    jsonReflect_ = true;
    call->type = type->constructed();
  }
  call->builtin = Builtin::FromJsonClass;
  return call;
}

} // namespace tz
