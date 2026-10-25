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
// compiling, the call is what it stands for (docs/decorators.md, plan step
// 5): `Reflect.construct(User, [a])` is `new User(a)`, `Reflect.get(user,
// "name")` reads the field, Reflect.get of a method gives its function
// without the description, Reflect.apply of that function calls the method
// when the checks would let the arguments through, and Reflect.construct of
// a known class with arguments known only when the program runs calls the
// function that checks them and makes the object. A class nothing else
// reaches by reflection is then not described at all.

#include "compiler/check/sema/sema.h"

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

Expr *stripImplicit(Expr *expr) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit())
      break;
    expr = conv->operand;
  }
  return expr;
}

// Whether every value of type `arg` passes the check that Reflect.apply makes
// of an argument for a parameter of type `param` (CodeGen::emitUnknownAs),
// so that a call known when compiling may skip it. Where the check is
// stricter than the types (an object literal, a union, whose member the
// check picks in its own order), it is kept.
bool reflectAccepts(const Type *arg, const Type *param) {
  if (param->isUnknown())
    return true;
  if (param->isOptional())
    return arg->isUndefined() ||
           reflectAccepts(arg->isOptional() ? arg->inner() : arg, param->inner());
  if (arg->isOptional() || arg->isUnion() || param->isUnion())
    return false;
  const Type *value = arg->isLiteral() ? arg->literalBase() : arg;
  switch (param->kind()) {
  case Type::Kind::Int:
    return value->kind() == Type::Kind::Int;
  case Type::Kind::Long:
    return value->kind() == Type::Kind::Int || value->kind() == Type::Kind::Long;
  case Type::Kind::Double:
    return value->kind() == Type::Kind::Int || value->kind() == Type::Kind::Long ||
           value->kind() == Type::Kind::Double;
  case Type::Kind::Bool:
    return value->isBool();
  case Type::Kind::String:
    return value->isString();
  case Type::Kind::Array:
  case Type::Kind::Function:
  case Type::Kind::Decimal:
  case Type::Kind::Result:
  case Type::Kind::Json:
    return arg == param; // a box of exactly the type
  case Type::Kind::ClassRef: {
    const ClassDecl *cls = arg->classRef();
    if (!cls || !cls->type)
      return false;
    if (param->classRef())
      return cls == param->classRef();
    const Type *made = param->constructed();
    return made->isAnyObject() || (made->classDecl() && cls->type->isSubtypeOf(made));
  }
  case Type::Kind::Object: {
    // An object of a class, which its number tells.
    const ClassDecl *cls = arg->classDecl();
    if (!cls || cls->literal || cls->builtin || !arg->isClass())
      return param->isDate() && arg->isDate();
    if (param->isAnyObject())
      return true;
    return (param->isClass() || param->isBase() || !param->bases().empty()) &&
           arg->isSubtypeOf(param);
  }
  default:
    return false;
  }
}

} // namespace

bool Sema::onlyChecked() const {
  return trial_ || llvm::any_of(functions_, [](const FunctionDecl *fn) {
           return (!fn->typeParams.empty() && !fn->isInstance) ||
                  (fn->cls && (fn->cls->isGeneric() || fn->cls->checkOnly));
         });
}

void Sema::noteReflectedConversion(const Type *from, const Type *to, const CastExpr *cast) {
  // Code that is never emitted reaches no class.
  if (onlyChecked())
    return;
  // A class that becomes some class known only when the program runs, which
  // `new` and Reflect.construct make objects of; unless a copy for the class
  // known when compiling drops the conversion (see exactClassArgs).
  if (to->nonOptional()->isConstructor()) {
    if (ClassDecl *cls = const_cast<ClassDecl *>(from->nonOptional()->classRef()))
      constructorCasts_[cast] = cls;
    return;
  }
  // Objects that code looks into only through reflection.
  if (to->nonOptional()->isAnyObject() || to->isUnknown() ||
      (to->nonOptional()->isArray() && to->nonOptional()->inner()->isUnknown()))
    classesIn(from, escaped_);
}

void Sema::finishReflection() {
  for (const auto &entry : constructorCasts_)
    entry.second->reflectNew = true;
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

FunctionDecl *Sema::knownReflectMethod(ClassDecl *cls, llvm::StringRef name, bool &several) {
  // The variants up the chain, as the description has them: a generic one
  // is a function for each copy, and Reflect.get gives none of them.
  std::vector<FunctionDecl *> methods = methodsNamed(cls, name, /*isStatic=*/false);
  llvm::erase_if(methods, [](const FunctionDecl *fn) { return !fn->typeParams.empty(); });
  if (methods.empty())
    return nullptr;
  for (FunctionDecl *fn : methods) {
    if (!fn->sym || fn->isAbstract || fn->access != Access::Public ||
        soleImplementation(cls, fn) != fn)
      return nullptr;
    const Type *ret = returnType(fn->sym);
    if (!ret || ret->isError())
      return nullptr;
  }
  // A class down the chain that declares a method of the name again, another
  // variant, would give its own.
  std::function<bool(const ClassDecl *)> declaresAgain = [&](const ClassDecl *c) {
    return llvm::any_of(c->children, [&](const ClassDecl *child) {
      return llvm::any_of(ownMethods(child),
                          [&](const FunctionDecl *m) { return m->name == name; }) ||
             declaresAgain(child);
    });
  };
  if (declaresAgain(cls))
    return nullptr;
  several = methods.size() > 1;
  return methods.front();
}

const FunctionDecl *Sema::heldReflectMethod(const Expr *f) {
  f = stripImplicit(const_cast<Expr *>(f));
  if (auto *name = dyn_cast<NameExpr>(f))
    return name->var ? name->var->reflectMethod : nullptr;
  // `Reflect.apply(Reflect.get(o, "m"), ...)`, whose object has no effects
  // to keep; of several variants, the arguments choose when it runs.
  if (auto *got = dyn_cast<CallExpr>(f); got && got->builtin == Builtin::ReflectMethod &&
                                         !severalVariants_.contains(got) &&
                                         isa<NameExpr>(stripImplicit(got->args[0])))
    return got->method->decl;
  return nullptr;
}

Expr *Sema::directReflectApply(CallExpr *call, const FunctionDecl *fn) {
  // `this`: an object of the method's class, which the function checks.
  Expr *self = stripImplicit(call->args[1]);
  const ClassDecl *cls = self->type->isOptional() ? nullptr : self->type->classDecl();
  if (!cls || cls->literal || !cls->derivesFrom(fn->cls))
    return nullptr;
  auto *list = dyn_cast<ArrayLitExpr>(call->args[2]);
  if (!list)
    return nullptr;
  FuncSymbol *sym = fn->sym;
  resolveParams(sym);
  std::vector<Expr *> args;
  for (const ArrayLitExpr::Element &e : list->elements) {
    if (e.spread || e.hole)
      return nullptr;
    args.push_back(stripImplicit(e.expr));
  }
  // As many as it takes, each of a type the check lets through.
  size_t fixed = sym->hasRest ? sym->params.size() - 1 : sym->params.size();
  if (args.size() < sym->minArgs || (!sym->hasRest && args.size() > fixed))
    return nullptr;
  for (size_t i = 0; i < args.size(); ++i) {
    const Type *param = i < fixed ? sym->params[i] : sym->params.back()->inner();
    if (!param || !reflectAccepts(args[i]->type, param))
      return nullptr;
  }
  // `self.m(args)`, of this very method, which no class down the chain
  // declares again.
  auto *member = ctx_.make<MemberExpr>(call->loc(), self, fn->name);
  member->type = functionType(sym);
  auto *direct = ctx_.make<CallExpr>(call->loc(), member, std::move(args));
  direct->method = sym;
  direct->type = returnType(sym);
  std::vector<bool> given(direct->args.size(), true);
  Expr *result =
      checkArguments(direct, sym->params, sym->minArgs, sym->hasRest, call->loc(), &given);
  if (!result->type->isVoid())
    return convert(result, Type::unknown());
  // Undefined, after the call.
  Expr *nothing = convert(checkExpr(ctx_.make<UndefinedExpr>(call->loc())), Type::unknown());
  auto *seq = ctx_.make<SequenceExpr>(call->loc(), std::vector<Expr *>{result, nothing});
  seq->type = Type::unknown();
  return seq;
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
    // `T extends object` in a generic function: its copies take the classes.
    const Type *base = apparentType(type->nonOptional());
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
    // An object of a class known when compiling, also where a copy of a
    // generic function passes its T on as an unknown, as the generic code
    // does (see noteForced).
    Expr *seen = stripImplicit(target);
    const Type *seenType = seen->type;
    ClassDecl *cls =
        seenType->isObject() && !seenType->isAnyObject() ? classOf(seenType) : nullptr;
    if (key && !seenType->isOptional() &&
        llvm::all_of(key->value, [](char16_t c) { return c < 128; })) {
      std::string name;
      for (char16_t c : key->value)
        name.push_back(char(c));
      if (cls)
        completeClass(cls);
      const ClassField *declared = cls ? findField(cls, name) : nullptr;
      bool open = !cls || (declared && declared->access == Access::Public);
      // A field: read as it is.
      if (object && !type->isOptional() && base->isObject() && !base->isAnyObject() &&
          base->field(name) && open) {
        auto *read = ctx_.make<MemberExpr>(
            key->loc(), ctx_.make<NameExpr>(object->loc(), object->name), ctx_.keepName(name));
        return convert(checkExpr(read), Type::unknown());
      }
      if (cls && !cls->literal && seenType->field(name) && open) {
        auto *read = ctx_.make<MemberExpr>(key->loc(), seen, ctx_.keepName(name));
        return convert(checkClassMember(read, nullptr), Type::unknown());
      }
      // A method: its function, as the description would give it.
      bool several = false;
      if (FunctionDecl *method = cls && !cls->literal && !seenType->field(name)
                                     ? knownReflectMethod(cls, name, several)
                                     : nullptr) {
        call->args.resize(1);
        target = seen;
        call->builtin = Builtin::ReflectMethod;
        call->method = method->sym;
        call->type = Type::unknown();
        if (several)
          severalVariants_.insert(call);
        return call;
      }
    }
    call->args[1] = checkArg(call->args[1], Type::string());
    // Objects of these classes are looked into by name.
    if (onlyChecked()) {
      // Never emitted.
    } else if (type->isUnknown() || base->isAnyObject() || !base->classDecl()) {
      reflectGetAny_ = true;
    } else {
      classesIn(base, reflectedObjects_);
    }
    target = convert(target, Type::unknown());
    reflectionUsed_ |= !onlyChecked();
    call->builtin = Builtin::ReflectGet;
    call->type = Type::unknown();
    return call;
  }
  if (fn == "apply") {
    if (!checkArity(call, member->loc(), 3, 3))
      return call;
    Expr *&f = call->args[0];
    f = checkExpr(f);
    const FunctionDecl *held = heldReflectMethod(f);
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
    if (held && !f->type->isError() && !self->type->isError() && !call->args[2]->type->isError())
      if (Expr *direct = directReflectApply(call, held))
        return direct;
    if (!onlyChecked()) {
      reflectionUsed_ = true;
      reflectGetAny_ = true; // the method of an object it got by name
    }
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
  // A class known when compiling, with arguments known only when the
  // program runs: the function of the class that checks them makes the
  // object, without its description. An abstract one fails when the program
  // runs, through the description, as JavaScript fails then.
  bool known = false;
  if (const ClassDecl *made = type->classRef()) {
    if (!made->type) {
      diags_.error(cls->loc(), "'" + made->name + "' of JavaScript cannot be made through "
                                                  "reflection");
      call->args[1] = checkReflectArgs(call->args[1]);
      return call;
    }
    known = !made->isAbstract;
    if (!known)
      cls = convert(cls, Type::constructorOf(made->type));
  }
  call->args[1] = checkReflectArgs(call->args[1]);
  calledFunction();
  reflectionUsed_ |= !known && !onlyChecked();
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
    if (!onlyChecked()) {
      reflectionUsed_ = true;
      jsonReflect_ = true;
    }
    call->type = type->constructed();
  }
  call->builtin = Builtin::FromJsonClass;
  return call;
}

} // namespace tz
