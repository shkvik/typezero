// Generic classes (docs/generics.md, section 5).
//
// A generic class is checked once with its type parameters opaque, as a
// generic function is (see sema_generic.cpp), and each list of type arguments
// makes an instance: `Box<int>` is a class of its own, parsed again from the
// text of `Box` (ClassDecl::source). Its bases, the types of its fields and
// the signatures of its methods are those of the generic class with the
// types substituted, never worked out again, so every instance agrees with
// the generic class; its bodies are checked with those types, and what our
// rules reject there (an object turned into a string, `<` on objects) is
// reported where the instance is first used. CodeGen emits each instance as
// a class written for its types: `Stack<int>` holds an `int[]`, and its
// methods are the code a hand-written IntStack would have.
//
// The static members belong to the generic class itself and cannot use its
// type parameters, as in TypeScript: every instance shares them. A class may
// extend an instance (`class IntStack extends Stack<int>`), and a generic
// class one made of its own type parameters (`class A<T> extends B<T>`): the
// instances of A extend those of B. A method that a class down the chain of
// one instance overrides is called through the method table in every
// instance, since the instances are made as the program is checked, and a
// call checked before one of them exists must work for it too.

#include "compiler/load/lexer/lexer.h"
#include "compiler/load/parser/parser.h"
#include "compiler/check/sema/sema.h"

#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

// How many instances of one generic class the instances being made may hold
// before they count as endless: `class Node<T> { next?: Node<T[]> }` needs
// Node<int>, Node<int[]>, Node<int[][]>...
constexpr unsigned kMaxClassChain = 16;

// "int, string"
std::string typeNames(llvm::ArrayRef<const Type *> types) {
  std::string text;
  for (const Type *t : types)
    text += (text.empty() ? "" : ", ") + t->name().str();
  return text;
}

// "T = int, U = string"
std::string bindings(llvm::ArrayRef<const Type *> params, llvm::ArrayRef<const Type *> args) {
  std::string text;
  for (size_t i = 0; i < params.size() && i < args.size(); ++i)
    text += (text.empty() ? "" : ", ") + params[i]->name().str() + " = " + args[i]->name().str();
  return text;
}

} // namespace

const ClassDecl *genericInstanceOf(const Type *type, const ClassDecl *generic) {
  for (const ClassDecl *c = type ? type->classDecl() : nullptr; c; c = c->parent)
    if (c == generic || c->generic == generic)
      return c;
  return nullptr;
}

//===----------------------------------------------------------------------===//
// Type parameters
//===----------------------------------------------------------------------===//

void Sema::declareClassTypeParams(ClassDecl *cls) {
  if (!cls->isGeneric() || !cls->typeArgs.empty())
    return;
  // In the file of the class, where the names its constraints use mean what
  // they mean there, and without the type parameters of any code around.
  std::vector<llvm::StringMap<Stmt *>> outer = typeScopes_;
  typeScopes_.resize(1);
  Module *here = module_;
  Module *home = declModules_.lookup(cls);
  bool moved = home && home != module_ && scopes_.size() == 1;
  if (moved)
    useModule(home);
  typeScopes_.emplace_back();
  std::vector<const Type *> defaults;
  cls->typeArgs = declareTypeParams(cls->typeParams, &defaults);
  typeScopes_.pop_back();
  if (moved)
    useModule(here);
  typeScopes_ = std::move(outer);
  classDefaults_[cls] = std::move(defaults);
  // `Box<T>` in messages, and the type of `this` in its methods.
  auto *type = const_cast<Type *>(cls->type);
  type->setTypeArgs(cls->typeArgs);
  type->rename(cls->name.str() + "<" + typeNames(cls->typeArgs) + ">");
  classInstances_[{cls, cls->typeArgs}] = cls;
}

void Sema::bindClassTypeParams(const ClassDecl *cls) {
  if (cls && !cls->typeParams.empty() && cls->typeParams.size() == cls->typeArgs.size())
    bindTypeParams(cls->typeParams, cls->typeArgs);
}

const Type *Sema::instanceType(const ClassDecl *inst, const Type *type) {
  if (!type || !inst->generic)
    return type;
  return substitute(type, inst->generic->typeArgs, inst->typeArgs);
}

//===----------------------------------------------------------------------===//
// Instances
//===----------------------------------------------------------------------===//

ClassDecl *Sema::instantiateClass(ClassDecl *generic, std::vector<const Type *> args, llvm::SMLoc use) {
  declareClassTypeParams(generic);
  // Without a place of its own (a type that substitute makes), errors are
  // reported at the generic class.
  llvm::SMLoc loc = use.isValid() ? use : generic->loc();
  // Copied: making instances below may add entries.
  std::vector<const Type *> params = generic->typeArgs;
  std::vector<const Type *> defaults = classDefaults_.lookup(generic);
  std::string name = generic->name.str();
  if (args.size() > params.size()) {
    diags_.error(loc, "'" + name + "' takes " + std::to_string(params.size()) + " type argument" +
                          (params.size() == 1 ? "" : "s") + ", but got " + std::to_string(args.size()));
    return nullptr;
  }
  bool emitter = generic->library && generic->name == "EventEmitter";
  for (size_t k = args.size(); k < params.size(); ++k) {
    if (emitter) {
      diags_.error(loc, "'EventEmitter' needs its event map here, e.g. 'EventEmitter<{ data: "
                        "[chunk: string]; end: [] }>', or the type of an emitter, e.g. 'typeof "
                        "bus'");
      return nullptr;
    }
    if (k >= defaults.size() || !defaults[k]) {
      std::string ints;
      for (size_t i = 0; i < params.size(); ++i)
        ints += i == 0 ? "int" : ", int";
      diags_.error(loc, "'" + name + "' needs " + std::to_string(params.size()) + " type argument" +
                            (params.size() == 1 ? "" : "s") + ", e.g. '" + name + "<" + ints + ">'");
      return nullptr;
    }
    // A default may use the type parameters before it.
    std::vector<const Type *> known = args;
    known.resize(params.size(), nullptr);
    args.push_back(substitute(defaults[k], params, known));
  }
  for (size_t k = 0; k < params.size(); ++k) {
    if (args[k]->isError())
      return nullptr;
    if (args[k]->isVoid() || args[k]->isUndefined()) {
      diags_.error(loc, "'" + args[k]->name() + "' cannot be a type argument of '" + name + "'");
      return nullptr;
    }
    if (!satisfies(args[k], params[k], params, args)) {
      const TypeParamDecl &decl = generic->typeParams[k];
      std::string constraint = params[k]->constraint() ? params[k]->constraint()->name().str() : "";
      for (const Type *t : params[k]->alternatives())
        constraint += (constraint.empty() ? "" : " | ") + t->name().str();
      if (decl.constraint && decl.constraint->kind == TypeRef::Kind::Name)
        constraint = decl.constraint->name.str();
      diags_.error(loc, "type '" + args[k]->name() + "' does not satisfy the constraint '" + constraint +
                            "' of '" + params[k]->name() + "' in '" + name + "'");
      return nullptr;
    }
  }
  // The event map of an emitter (sema_events.cpp).
  if (emitter && !checkEventMap(args[0], loc))
    return nullptr;
  auto key = std::make_pair(static_cast<const ClassDecl *>(generic), args);
  if (auto found = classInstances_.find(key); found != classInstances_.end()) {
    // The first place in the program that names the instance is where its
    // errors are reported.
    if (Instance *check = classChecks_.lookup(found->second); check && !check->callLoc.isValid())
      check->callLoc = use;
    return found->second;
  }
  if (llvm::count_if(filling_, [&](const ClassDecl *c) { return c->generic == generic; }) >= kMaxClassChain) {
    if (endless_.insert(generic).second)
      diags_.error(generic->loc(), "generic class '" + name + "' uses itself with type arguments that keep "
                                   "growing, so each instance of it needs another, without end; a "
                                   "generic class can use itself only with its own type parameters");
    return nullptr;
  }

  // A copy of the declaration. It was parsed once without errors.
  bool muted = diags_.setMuted(true);
  Lexer lexer(generic->source, diags_);
  Parser parser(lexer, ctx_, diags_, /*library=*/generic->library);
  ClassDecl *inst = parser.parseClassCopy();
  diags_.setMuted(muted);
  inst->generic = generic;
  inst->typeArgs = args;
  inst->isAbstract = generic->isAbstract;
  inst->library = generic->library;
  inst->checkOnly = llvm::any_of(args, [](const Type *t) { return mentionsParam(t); });
  inst->resolved = true;
  inst->declared = true;
  inst->id = ++classCount_;
  Type *type = Type::classType(name + "<" + typeNames(args) + ">", inst);
  type->setTypeArgs(args);
  inst->type = type;
  classes_.push_back(inst);
  generic->instances.push_back(inst);
  classInstances_[key] = inst;
  if (Module *home = declModules_.lookup(generic))
    declModules_[inst] = home;

  // The check of its bodies, by the place that first needs it.
  Instance *check = nullptr;
  if (!inst->checkOnly) {
    check = &instanceStore_.emplace_back();
    check->cls = inst;
    check->callLoc = use;
    check->requester = checkingInstance_;
    classChecks_[inst] = check;
  }
  if (!methodsDeclared_) {
    unfilled_.push_back(inst); // its generic class may not have its methods yet
  } else {
    filling_.push_back(inst);
    fillInstance(inst);
    filling_.pop_back();
    if (classesLinked_)
      linkInstance(inst);
  }
  if (check) {
    // From the body of another instance: that one fails if this one does.
    if (checkingInstance_) {
      checkClassInstance(check);
      if (check->failed)
        reportClassInstance(check, loc);
    } else {
      pendingInstances_.push_back(check);
    }
  }
  return inst;
}

void Sema::declareInstanceMember(ClassDecl *inst, FunctionDecl *fn, FunctionDecl *generic) {
  FuncSymbol *of = generic->sym;
  FuncSymbol *sym = ctx_.newFunc();
  sym->name = fn->name;
  sym->loc = fn->loc();
  sym->decl = fn;
  fn->sym = sym;
  fn->generic = generic;
  fn->variant = generic->variant;
  fn->overridden = generic->overridden;
  fn->openToLate = generic->openToLate;
  if (!of)
    return; // an error in the generic class
  auto sub = [&](const Type *t) { return instanceType(inst, t); };
  // A generic method keeps its own type parameters; one that a parameter
  // `xs: Iterable<T>` makes is one of the values of this instance.
  std::vector<const Type *> own = of->typeParams, mine = of->typeParams;
  for (const Type *&p : mine)
    if (p->iterated() && typeMentions(p->iterated(), inst->generic->typeArgs)) {
      Type *param = Type::param("Iterable<" + sub(p->iterated())->name().str() + ">");
      param->setIterated(sub(p->iterated()));
      p = param;
    }
  auto sub2 = [&](const Type *t) { return t ? substitute(sub(t), own, mine) : nullptr; };
  fn->typeArgs = mine;
  sym->typeParams = mine;
  for (const Type *d : of->typeDefaults)
    sym->typeDefaults.push_back(sub2(d));
  for (const Type *p : of->params)
    sym->params.push_back(sub2(p));
  sym->pendingParams = of->pendingParams;
  for (size_t i = 0; i < fn->params.size() && i < generic->params.size(); ++i)
    fn->params[i].declaredType = sub2(generic->params[i].declaredType);
  // The parameters an arrow method takes without naming them (see
  // declareTyped).
  for (size_t i = fn->params.size(); i < generic->params.size(); ++i) {
    fn->params.push_back(generic->params[i]);
    fn->params.back().declaredType = sub2(generic->params[i].declaredType);
  }
  // The hidden type parameters an instance's copy of the method declares.
  for (size_t i = fn->typeParams.size(); i < generic->typeParams.size(); ++i)
    fn->typeParams.push_back(generic->typeParams[i]);
  sym->minArgs = of->minArgs;
  sym->hasRest = of->hasRest;
  sym->ret = sub2(of->ret); // null: found from the generic method (see inferReturnType)
  sym->discardsResult = of->discardsResult;
  sym->predicateParam = of->predicateParam;
  sym->asserts = of->asserts;
  sym->predicateType = sub2(of->predicateType);
  if (Module *home = declModules_.lookup(generic))
    declModules_[fn] = home;
}

void Sema::fillInstance(ClassDecl *inst) {
  ClassDecl *generic = inst->generic;
  auto sub = [&](const Type *t) { return instanceType(inst, t); };
  // Its bases, whatever code makes the instance: CodeGen lays out the
  // objects of every instance it emits (see addBase).
  auto addInstanceBase = [&](const Type *base) {
    addBase(inst->type, base);
    if (inst->isEmitted() && !llvm::is_contained(file_->subtypes, inst->type))
      file_->subtypes.push_back(inst->type);
  };
  // The class it extends and the interfaces it implements, those of the
  // generic class with its types: `class A<T> extends B<T>` makes A<int>
  // extend B<int>.
  if (const ClassDecl *parent = generic->parent) {
    if (ClassDecl *cls = classOf(sub(parent->type))) {
      inst->parent = cls;
      cls->children.push_back(inst);
      addInstanceBase(cls->type);
    }
  }
  for (const Type *iface : generic->implemented) {
    const Type *type = sub(iface);
    if (!type->isError() && !llvm::is_contained(inst->implemented, type)) {
      inst->implemented.push_back(type);
      addInstanceBase(type);
    }
  }
  // A Duplex of node:stream is a Writable too (sema_stream.cpp).
  addStreamBases(inst, inst->loc());
  // The fields, in the order of the generic class: the copy is parsed from
  // its text. The static ones are the generic class's.
  for (size_t i = 0; i < inst->fields.size() && i < generic->fields.size(); ++i) {
    ClassField &f = inst->fields[i];
    const ClassField &g = generic->fields[i];
    f.optional = g.optional;
    f.redeclared = g.redeclared;
    if (f.isStatic)
      continue;
    f.resolved = sub(g.resolved); // null: found when the generic class is complete
    if (g.method) {
      FunctionDecl *fn = cast<FunctionExpr>(f.init)->fn;
      fn->name = g.method->name;
      fn->cls = inst;
      fn->access = g.method->access;
      fn->isOverride = g.method->isOverride;
      fn->isExpression = false;
      f.method = fn;
    }
  }
  for (size_t i = 0; i < inst->methods.size() && i < generic->methods.size(); ++i)
    if (!generic->methods[i]->isStatic)
      declareInstanceMember(inst, inst->methods[i], generic->methods[i]);
  for (size_t i = 0; i < inst->constructors.size() && i < generic->constructors.size(); ++i)
    declareInstanceMember(inst, inst->constructors[i], generic->constructors[i]);
  for (size_t i = 0; i < inst->fields.size() && i < generic->fields.size(); ++i)
    if (inst->fields[i].method && generic->fields[i].method)
      declareInstanceMember(inst, inst->fields[i].method, generic->fields[i].method);
  // The initializers of the fields, as `init` (see declareMethods).
  if (generic->init) {
    std::vector<Stmt *> sets;
    for (const ClassField &f : inst->fields) {
      if (f.isStatic || !f.init || f.method)
        continue;
      auto *target = ctx_.make<MemberExpr>(f.loc, ctx_.make<NameExpr>(f.loc, "this"), f.name);
      auto *set = ctx_.make<AssignExpr>(f.loc, std::nullopt, target, f.init);
      sets.push_back(ctx_.make<ExprStmt>(f.loc, set));
    }
    auto *init = ctx_.make<FunctionDecl>(inst->loc(), "init");
    init->cls = inst;
    init->body = ctx_.make<BlockStmt>(inst->loc());
    init->body->body = std::move(sets);
    declareInstanceMember(inst, init, generic->init);
    init->sym->ret = Type::voidTy();
    inst->init = init;
  }
  // Variants that these types make the same (`add(x: T)` and `add(x:
  // string)` with T = string): the check of the instance reports them.
  std::vector<Diagnostics::Captured> errors;
  std::vector<Diagnostics::Captured> *outer = diags_.setCapture(&errors);
  checkVariants(inst);
  diags_.setCapture(outer);
  if (!errors.empty())
    if (Instance *check = classChecks_.lookup(inst); check && !check->failed) {
      check->failed = true;
      check->message = errors.front().message;
      check->errorLoc = errors.front().loc;
    }
}

void Sema::linkInstance(ClassDecl *inst) {
  // What the generic class reported is not reported again.
  bool muted = diags_.setMuted(true);
  linkOverrides(inst);
  diags_.setMuted(muted);
  buildVtable(inst);
}

void Sema::markOverridden(FunctionDecl *fn) {
  fn->overridden = true;
  FunctionDecl *generic = fn->generic ? fn->generic : fn;
  ClassDecl *cls = generic->cls;
  if (!cls || !cls->isGeneric())
    return;
  generic->overridden = true;
  for (ClassDecl *inst : cls->instances)
    for (FunctionDecl *m : ownMethods(inst))
      if (m->generic == generic)
        m->overridden = true;
}

//===----------------------------------------------------------------------===//
// Checks of the instances
//===----------------------------------------------------------------------===//

void Sema::checkClassInstance(Instance *instance) {
  ClassDecl *inst = instance->cls;
  if (instance->checked)
    return;
  instance->checking = true;
  std::vector<Diagnostics::Captured> errors;
  if (!brokenClasses_.contains(inst->generic)) {
    completeClass(inst);
    std::vector<FunctionDecl *> bodies;
    if (inst->init)
      bodies.push_back(inst->init);
    bodies.insert(bodies.end(), inst->constructors.begin(), inst->constructors.end());
    // A generic method is checked in the copies of it that calls need.
    for (FunctionDecl *fn : ownMethods(inst))
      if (!fn->isAbstract && fn->typeParams.empty())
        bodies.push_back(fn);
    for (FunctionDecl *fn : bodies) {
      if (fn->checked || !fn->sym)
        continue;
      if (!fn->sym->ret)
        inferReturnType(fn->sym); // that of the generic method
      BodyState saved = enterBody(fn);
      checkingInstance_ = instance;
      diags_.setCapture(&errors);
      flow_ = Flow();
      checkFunctionBody(fn);
      leaveBody(saved);
    }
  }
  instance->checking = false;
  instance->checked = true;
  // The variants that the types make the same are reported first (see
  // fillInstance).
  if (errors.empty() || instance->failed)
    return;
  instance->failed = true;
  instance->message = errors.front().message;
  instance->errorLoc = errors.front().loc;
}

void Sema::reportClassInstance(Instance *instance, llvm::SMLoc use) {
  llvm::SMLoc loc = use.isValid() ? use : instance->cls->generic->loc();
  if (diags_.capture()) {
    // In the body of another instance: this use is where that one fails.
    failedCalls_[loc.getPointer()] = instance;
    diags_.error(loc, instance->message);
    return;
  }
  const ClassDecl *inst = instance->cls;
  diags_.error(loc, "cannot use '" + inst->name + "' with " + bindings(inst->generic->typeArgs, inst->typeArgs) +
                        ": " + instance->message);
  const Instance *at = instance;
  llvm::SmallPtrSet<const char *, 8> shown;
  auto nameOf = [](const Instance *i) { return i->cls ? i->cls->name : i->generic->name; };
  while (at && at->errorLoc.isValid() && shown.insert(at->errorLoc.getPointer()).second) {
    const Instance *next = failedCalls_.lookup(at->errorLoc.getPointer());
    if (next)
      diags_.note(at->errorLoc, "in '" + nameOf(at) + "', which uses '" + nameOf(next) + "' here");
    else
      diags_.note(at->errorLoc, "in '" + nameOf(at) + "', here");
    at = next;
  }
}

//===----------------------------------------------------------------------===//
// new
//===----------------------------------------------------------------------===//

Expr *Sema::checkGenericNew(NewExpr *expr, ClassDecl *generic, const Type *expected) {
  expr->type = Type::error();
  auto checkArgsLoosely = [&](std::vector<Expr *> &args) {
    for (Expr *&arg : args)
      if (!arg->type)
        arg = checkExpr(arg);
  };
  // `new EventEmitter()`, with its event map or without (sema_events.cpp).
  if (generic->library && generic->name == "EventEmitter")
    if (Expr *emitter = checkEmitterNew(expr, generic, expected))
      return emitter;
  // `new Readable({...})` (sema_stream.cpp).
  if (generic->library && isStreamClass(generic))
    if (Expr *stream = checkStreamNew(expr, generic, expected))
      return stream;
  // `new Box<int>(1)`: the instance, as any class.
  if (!expr->typeArgs.empty()) {
    std::vector<const Type *> args;
    for (const TypeRef &ref : expr->typeArgs) {
      args.push_back(resolveType(ref, /*allowVoid=*/false));
      if (args.back()->isError()) {
        checkArgsLoosely(expr->args);
        return expr;
      }
    }
    ClassDecl *inst = instantiateClass(generic, std::move(args), expr->loc());
    if (!inst) {
      checkArgsLoosely(expr->args);
      return expr;
    }
    expr->typeArgs.clear();
    return checkClassNew(expr, inst, expected);
  }
  if (!generic->declared && loading_ && functions_.empty() && declModules_.lookup(generic) == loading_)
    diags_.error(expr->loc(), "class '" + generic->name +
                                  "' is used before its declaration, which JavaScript has not run "
                                  "yet here (a ReferenceError); move the class up");
  if (generic->isAbstract)
    diags_.error(expr->loc(), "class '" + generic->name + "' is abstract and cannot be created: create "
                                  "an object of a class that extends it");
  // The type arguments come from the constructor that the arguments choose,
  // as from a generic function whose result is `Box<T>`.
  std::vector<FunctionDecl *> ctors = constructorsOf(generic);
  FunctionDecl *ctor = ctors.size() == 1 ? ctors[0] : nullptr;
  if (ctors.size() > 1) {
    std::vector<FunctionDecl *> fit;
    for (FunctionDecl *c : ctors) {
      resolveParams(c->sym);
      const FuncSymbol *sym = c->sym;
      if (expr->args.size() >= sym->minArgs && (sym->hasRest || expr->args.size() <= sym->params.size()))
        fit.push_back(c);
    }
    if (fit.size() != 1) {
      diags_.error(expr->loc(), "cannot infer the type arguments of '" + generic->name + "': " +
                                    (fit.empty() ? "no constructor takes " : "several constructors take ") +
                                    std::to_string(expr->args.size()) + " argument" +
                                    (expr->args.size() == 1 ? "" : "s") + "; give them, e.g. 'new " +
                                    generic->name + "<int>(...)'");
      checkArgsLoosely(expr->args);
      return expr;
    }
    ctor = fit[0];
  }
  FuncSymbol *&fn = constructorFunctions_[{generic, ctor}];
  if (!fn) {
    fn = ctx_.newFunc();
    fn->name = ctx_.keepName("new " + generic->name.str());
    fn->loc = generic->loc();
    auto *decl = ctx_.make<FunctionDecl>(generic->loc(), fn->name);
    decl->typeParams = generic->typeParams;
    decl->sym = fn;
    fn->decl = decl;
    fn->typeParams = generic->typeArgs;
    fn->typeDefaults = classDefaults_.lookup(generic);
    if (ctor) {
      resolveParams(ctor->sym);
      fn->params = ctor->sym->params;
      fn->minArgs = ctor->sym->minArgs;
      fn->hasRest = ctor->sym->hasRest;
    }
    fn->ret = generic->type;
  }
  auto *call = ctx_.make<CallExpr>(expr->loc(), nullptr, std::move(expr->args));
  expr->ctor = call;
  std::vector<const Type *> bound;
  if (!bindGenericCall(call, fn, expr->loc(), expected, {}, bound))
    return expr;
  ClassDecl *inst = instantiateClass(generic, bound, expr->loc());
  if (!inst)
    return expr;
  completeClass(inst);
  expr->cls = inst;
  call->type = Type::voidTy();
  // The constructor of the instance that is a copy of the one chosen: the
  // same parameters, with the types of the instance.
  if (ctor) {
    std::vector<FunctionDecl *> mine = constructorsOf(inst);
    auto at = llvm::find(ctors, ctor) - ctors.begin();
    if (size_t(at) < mine.size())
      call->func = mine[size_t(at)]->sym;
    checkAccess(ctor->cls, ctor->access, "constructor", expr->loc());
  }
  for (const ClassDecl *c = inst; c; c = c->parent)
    if (c->init || !c->constructors.empty()) {
      calledFunction(); // the code of the class runs
      break;
    }
  expr->type = inst->type;
  return expr;
}

} // namespace tz
