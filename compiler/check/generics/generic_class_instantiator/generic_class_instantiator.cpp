#include "compiler/check/generics/generic_class_instantiator/generic_class_instantiator.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/check/generics/type_params/type_params.h"
#include "compiler/check/generics/generic_class_checker/generic_class_checker.h"
#include "compiler/check/classes/class_bases/class_bases.h"
#include "compiler/check/classes/class_links/class_links.h"
#include "compiler/library/events/event_maps/event_maps.h"
#include "compiler/library/stream/chunk_types/chunk_types.h"
#include "compiler/load/lexer/lexer.h"
#include "compiler/load/parser/parser.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

namespace {

// How many instances of one generic class the instances being made may hold
// before they count as endless: `class Node<T> { next?: Node<T[]> }` needs
// Node<int>, Node<int[]>, Node<int[][]>...
constexpr unsigned kMaxClassChain = 16;

// "int, string"
std::string typeNames(llvm::ArrayRef<const Type *> types) {
  std::string text;
  for (const Type *t : types) {
    text += (text.empty() ? "" : ", ") + t->name().str();
  }

  return text;
}

} // namespace

GenericClassInstantiator::GenericClassInstantiator(Sema &sema) : sema_(sema) {}

ClassDecl *GenericClassInstantiator::instantiateClass(ClassDecl *generic, std::vector<const Type *> args,
                                  llvm::SMLoc use) {
  sema_.genericClassChecker().declareClassTypeParams(generic);
  // Without a place of its own (a type that substitute makes), errors are
  // reported at the generic class.
  llvm::SMLoc loc = use.isValid() ? use : generic->loc();
  // Copied: making instances below may add entries.
  std::vector<const Type *> params = generic->typeArgs;
  std::vector<const Type *> defaults = sema_.classDefaults_.lookup(generic);
  std::string name = generic->name.str();
  if (args.size() > params.size()) {
    sema_.diags_.error(loc, "'" + name + "' takes " + std::to_string(params.size()) + " type argument" +
                          (params.size() == 1 ? "" : "s") + ", but got " +
                          std::to_string(args.size()));
    return nullptr;
  }
  bool emitter = generic->library && generic->name == "EventEmitter";
  for (size_t k = args.size(); k < params.size(); ++k) {
    if (emitter) {
      sema_.diags_.error(loc, "'EventEmitter' needs its event map here, e.g. 'EventEmitter<{ data: "
                        "[chunk: string]; end: [] }>', or the type of an emitter, e.g. 'typeof "
                        "bus'");
      return nullptr;
    }
    if (k >= defaults.size() || !defaults[k]) {
      std::string ints;
      for (size_t i = 0; i < params.size(); ++i) {
        ints += i == 0 ? "int" : ", int";
      }
      sema_.diags_.error(loc, "'" + name + "' needs " + std::to_string(params.size()) + " type argument" +
                            (params.size() == 1 ? "" : "s") + ", e.g. '" + name + "<" + ints +
                            ">'");
      return nullptr;
    }
    // A default may use the type parameters before it.
    std::vector<const Type *> known = args;
    known.resize(params.size(), nullptr);
    args.push_back(sema_.typeParams().substitute(defaults[k], params, known));
  }
  for (size_t k = 0; k < params.size(); ++k) {
    if (args[k]->isError()) {
      return nullptr;
    }
    if (args[k]->isVoid() || args[k]->isUndefined()) {
      sema_.diags_.error(loc, "'" + args[k]->name() + "' cannot be a type argument of '" + name + "'");

      return nullptr;
    }
    if (!sema_.typeParams().satisfies(args[k], params[k], params, args)) {
      const TypeParamDecl &decl = generic->typeParams[k];
      std::string constraint = params[k]->constraint() ? params[k]->constraint()->name().str() : "";
      for (const Type *t : params[k]->alternatives()) {
        constraint += (constraint.empty() ? "" : " | ") + t->name().str();
      }
      if (decl.constraint && decl.constraint->kind == TypeRef::Kind::Name) {
        constraint = decl.constraint->name.str();
      }
      sema_.diags_.error(loc, "type '" + args[k]->name() + "' does not satisfy the constraint '" +
                            constraint + "' of '" + params[k]->name() + "' in '" + name + "'");
      return nullptr;
    }
  }
  // The event map of an emitter (sema_events.cpp).
  if (emitter && !sema_.eventMaps().checkEventMap(args[0], loc)) {
    return nullptr;
  }
  auto key = std::make_pair(static_cast<const ClassDecl *>(generic), args);
  if (auto found = sema_.classInstances_.find(key); found != sema_.classInstances_.end()) {
    // The first place in the program that names the instance is where its
    // errors are reported.
    if (Sema::Instance *check = classChecks_.lookup(found->second); check && !check->callLoc.isValid()) {
      check->callLoc = use;
    }
    return found->second;
  }
  if (llvm::count_if(sema_.filling_, [&](const ClassDecl *c) { return c->generic == generic; }) >=
      kMaxClassChain) {
    if (endless_.insert(generic).second) {
      sema_.diags_.error(generic->loc(),
                   "generic class '" + name +
                       "' uses itself with type arguments that keep "
                       "growing, so each instance of it needs another, without end; a "
                       "generic class can use itself only with its own type parameters");
    }

    return nullptr;
  }

  // A copy of the declaration. It was parsed once without errors.
  bool muted = sema_.diags_.setMuted(true);
  Lexer lexer(generic->source, sema_.diags_);
  Parser parser(lexer, sema_.ctx_, sema_.diags_, /*library=*/generic->library);
  ClassDecl *inst = parser.parseClassCopy();
  sema_.diags_.setMuted(muted);
  inst->generic = generic;
  inst->typeArgs = args;
  inst->isAbstract = generic->isAbstract;
  inst->library = generic->library;
  inst->checkOnly = llvm::any_of(args, [](const Type *t) { return mentionsParam(t); });
  inst->resolved = true;
  inst->declared = true;
  inst->id = ++sema_.classCount_;
  Type *type = Type::classType(name + "<" + typeNames(args) + ">", inst);
  type->setTypeArgs(args);
  inst->type = type;
  sema_.classes_.push_back(inst);
  generic->instances.push_back(inst);
  sema_.classInstances_[key] = inst;
  if (Module *home = sema_.declModules_.lookup(generic)) {
    sema_.declModules_[inst] = home;
  }

  // The check of its bodies, by the place that first needs it.
  Sema::Instance *check = nullptr;
  if (!inst->checkOnly) {
    check = &sema_.instanceStore_.emplace_back();
    check->cls = inst;
    check->callLoc = use;
    check->requester = sema_.checkingInstance_;
    classChecks_[inst] = check;
  }
  if (!sema_.methodsDeclared_) {
    sema_.unfilled_.push_back(inst); // its generic class may not have its methods yet
  } else {
    sema_.filling_.push_back(inst);
    fillInstance(inst);
    sema_.filling_.pop_back();
    if (sema_.classesLinked_) {
      linkInstance(inst);
    }
  }
  if (check) {
    // From the body of another instance: that one fails if this one does.
    if (sema_.checkingInstance_) {
      sema_.genericClassChecker().checkClassInstance(check);
      if (check->failed) {
        sema_.genericClassChecker().reportClassInstance(check, loc);
      }
    } else {
      sema_.pendingInstances_.push_back(check);
    }
  }

  return inst;
}

void GenericClassInstantiator::declareInstanceMember(ClassDecl *inst, FunctionDecl *fn, FunctionDecl *generic) {
  FuncSymbol *of = generic->sym;
  FuncSymbol *sym = sema_.ctx_.newFunc();
  sym->name = fn->name;
  sym->loc = fn->loc();
  sym->decl = fn;
  fn->sym = sym;
  fn->generic = generic;
  fn->variant = generic->variant;
  fn->overridden = generic->overridden;
  fn->openToLate = generic->openToLate;
  if (!of) {
    return; // an error in the generic class
  }
  auto sub = [&](const Type *t) { return sema_.genericClassChecker().instanceType(inst, t); };
  // A generic method keeps its own type parameters; one that a parameter
  // `xs: Iterable<T>` makes is one of the values of this instance.
  std::vector<const Type *> own = of->typeParams, mine = of->typeParams;
  for (const Type *&p : mine) {
    if (p->iterated() && typeMentions(p->iterated(), inst->generic->typeArgs)) {
      Type *param = Type::param("Iterable<" + sub(p->iterated())->name().str() + ">");
      param->setIterated(sub(p->iterated()));
      p = param;
    }
  }
  auto sub2 = [&](const Type *t) { return t ? sema_.typeParams().substitute(sub(t), own, mine) : nullptr; };
  fn->typeArgs = mine;
  sym->typeParams = mine;
  for (const Type *d : of->typeDefaults) {
    sym->typeDefaults.push_back(sub2(d));
  }
  for (const Type *p : of->params) {
    sym->params.push_back(sub2(p));
  }
  sym->pendingParams = of->pendingParams;
  for (size_t i = 0; i < fn->params.size() && i < generic->params.size(); ++i) {
    fn->params[i].declaredType = sub2(generic->params[i].declaredType);
  }
  // The parameters an arrow method takes without naming them (see
  // declareTyped).
  for (size_t i = fn->params.size(); i < generic->params.size(); ++i) {
    fn->params.push_back(generic->params[i]);
    fn->params.back().declaredType = sub2(generic->params[i].declaredType);
  }
  // The hidden type parameters an instance's copy of the method declares.
  for (size_t i = fn->typeParams.size(); i < generic->typeParams.size(); ++i) {
    fn->typeParams.push_back(generic->typeParams[i]);
  }
  sym->minArgs = of->minArgs;
  sym->hasRest = of->hasRest;
  sym->ret = sub2(of->ret); // null: found from the generic method (see inferReturnType)
  sym->discardsResult = of->discardsResult;
  sym->predicateParam = of->predicateParam;
  sym->asserts = of->asserts;
  sym->predicateType = sub2(of->predicateType);
  if (Module *home = sema_.declModules_.lookup(generic)) {
    sema_.declModules_[fn] = home;
  }
}

void GenericClassInstantiator::fillInstance(ClassDecl *inst) {
  ClassDecl *generic = inst->generic;
  auto sub = [&](const Type *t) { return sema_.genericClassChecker().instanceType(inst, t); };
  // Its bases, whatever code makes the instance: CodeGen lays out the
  // objects of every instance it emits (see addBase).
  auto addInstanceBase = [&](const Type *base) {
    sema_.classBases().addBase(inst->type, base);
    if (inst->isEmitted() && !llvm::is_contained(sema_.file_->subtypes, inst->type)) {
      sema_.file_->subtypes.push_back(inst->type);
    }
  };
  // The class it extends and the interfaces it implements, those of the
  // generic class with its types: `class A<T> extends B<T>` makes A<int>
  // extend B<int>.
  if (const ClassDecl *parent = generic->parent) {
    if (ClassDecl *cls = ClassDeclarer::classOf(sub(parent->type))) {
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
  sema_.chunkTypes().addStreamBases(inst, inst->loc());
  // The fields, in the order of the generic class: the copy is parsed from
  // its text. The static ones are the generic class's.
  for (size_t i = 0; i < inst->fields.size() && i < generic->fields.size(); ++i) {
    ClassField &f = inst->fields[i];
    const ClassField &g = generic->fields[i];
    f.optional = g.optional;
    f.redeclared = g.redeclared;
    if (f.isStatic) {
      continue;
    }
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
  for (size_t i = 0; i < inst->methods.size() && i < generic->methods.size(); ++i) {
    if (!generic->methods[i]->isStatic) {
      declareInstanceMember(inst, inst->methods[i], generic->methods[i]);
    }
  }
  for (size_t i = 0; i < inst->constructors.size() && i < generic->constructors.size(); ++i) {
    declareInstanceMember(inst, inst->constructors[i], generic->constructors[i]);
  }
  for (size_t i = 0; i < inst->fields.size() && i < generic->fields.size(); ++i) {
    if (inst->fields[i].method && generic->fields[i].method) {
      declareInstanceMember(inst, inst->fields[i].method, generic->fields[i].method);
    }
  }
  // The initializers of the fields, as `init` (see declareMethods).
  if (generic->init) {
    std::vector<Stmt *> sets;
    for (const ClassField &f : inst->fields) {
      if (f.isStatic || !f.init || f.method) {
        continue;
      }
      auto *target = sema_.ctx_.make<MemberExpr>(f.loc, sema_.ctx_.make<NameExpr>(f.loc, "this"), f.name);
      auto *set = sema_.ctx_.make<AssignExpr>(f.loc, std::nullopt, target, f.init);
      sets.push_back(sema_.ctx_.make<ExprStmt>(f.loc, set));
    }
    auto *init = sema_.ctx_.make<FunctionDecl>(inst->loc(), "init");
    init->cls = inst;
    init->body = sema_.ctx_.make<BlockStmt>(inst->loc());
    init->body->body = std::move(sets);
    declareInstanceMember(inst, init, generic->init);
    init->sym->ret = Type::voidTy();
    inst->init = init;
  }
  // Variants that these types make the same (`add(x: T)` and `add(x:
  // string)` with T = string): the check of the instance reports them.
  std::vector<Diagnostics::Captured> errors;
  std::vector<Diagnostics::Captured> *outer = sema_.diags_.setCapture(&errors);
  sema_.classDeclarer().checkVariants(inst);
  sema_.diags_.setCapture(outer);
  if (!errors.empty()) {
    if (Sema::Instance *check = classChecks_.lookup(inst); check && !check->failed) {
      check->failed = true;
      check->message = errors.front().message;
      check->errorLoc = errors.front().loc;
    }
  }
}

void GenericClassInstantiator::linkInstance(ClassDecl *inst) {
  // What the generic class reported is not reported again.
  bool muted = sema_.diags_.setMuted(true);
  sema_.classLinks().linkOverrides(inst);
  sema_.diags_.setMuted(muted);
  sema_.classLinks().buildVtable(inst);
}

void GenericClassInstantiator::markOverridden(FunctionDecl *fn) {
  fn->overridden = true;
  FunctionDecl *generic = fn->generic ? fn->generic : fn;
  ClassDecl *cls = generic->cls;
  if (!cls || !cls->isGeneric()) {
    return;
  }
  generic->overridden = true;
  for (ClassDecl *inst : cls->instances) {
    for (FunctionDecl *m : ClassLinks::ownMethods(inst)) {
      if (m->generic == generic) {
        m->overridden = true;
      }
    }
  }
}

} // namespace tz
