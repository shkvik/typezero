#include "compiler/check/generics/generic_class_instantiator/generic_class_instantiator.h"
#include "compiler/check/classes/class_bases/class_bases.h"
#include "compiler/check/classes/class_links/class_links.h"
#include "compiler/check/generics/generic_class_checker/generic_class_checker.h"
#include "compiler/check/generics/type_params/type_params.h"
#include "compiler/declare/class_declarer/class_declarer.h"
#include "compiler/library/events/event_maps/event_maps.h"
#include "compiler/library/stream/chunk_types/chunk_types.h"
#include "compiler/load/lexer/lexer.h"
#include "compiler/load/parser/parser.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;

namespace tz {

namespace {

constexpr unsigned kMaxClassChain = 16;

}

std::string GenericClassInstantiator::typeNames(llvm::ArrayRef<const Type *> types) {
  std::string text;
  for (const Type *t : types) {
    text += (text.empty() ? "" : ", ") + t->name().str();
  }

  return text;
}

GenericClassInstantiator::GenericClassInstantiator(Checker &owner)
    : checker(owner) {}

ClassDecl *GenericClassInstantiator::instantiateClass(ClassDecl *generic,
                                                      std::vector<const Type *> args,
                                                      llvm::SMLoc use) {
  this->checker.genericClassChecker().declareClassTypeParams(generic);
  llvm::SMLoc loc = use.isValid() ? use : generic->loc();
  std::vector<const Type *> params = generic->typeArgs;
  std::vector<const Type *> defaults = this->checker.classDefaults_.lookup(generic);
  std::string name = generic->name.str();
  if (args.size() > params.size()) {
    this->checker.diags_.error(loc, "'" + name + "' takes " + std::to_string(params.size()) +
                                        " type argument" + (params.size() == 1 ? "" : "s") +
                                        ", but got " + std::to_string(args.size()));
    return nullptr;
  }
  bool emitter = generic->library() && generic->name == "EventEmitter";
  for (size_t k = args.size(); k < params.size(); ++k) {
    if (emitter) {
      this->checker.diags_.error(
          loc, "'EventEmitter' needs its event map here, e.g. 'EventEmitter<{ data: "
               "[chunk: string]; end: [] }>', or the type of an emitter, e.g. 'typeof "
               "bus'");
      return nullptr;
    }
    if (k >= defaults.size() || !defaults[k]) {
      std::string ints;
      for (size_t i = 0; i < params.size(); ++i) {
        ints += i == 0 ? "int" : ", int";
      }
      this->checker.diags_.error(loc, "'" + name + "' needs " + std::to_string(params.size()) +
                                          " type argument" + (params.size() == 1 ? "" : "s") +
                                          ", e.g. '" + name + "<" + ints + ">'");
      return nullptr;
    }
    std::vector<const Type *> known = args;
    known.resize(params.size(), nullptr);
    args.push_back(this->checker.typeParams().substitute(defaults[k], params, known));
  }
  for (size_t k = 0; k < params.size(); ++k) {
    if (args[k]->isError()) {
      return nullptr;
    }
    if (args[k]->isUndefined()) {
      this->checker.diags_.error(loc, "'" + args[k]->name() + "' cannot be a type argument of '" +
                                          name + "'");

      return nullptr;
    }
    if (!this->checker.typeParams().satisfies(args[k], params[k], params, args)) {
      const TypeParamDecl &decl = generic->typeParams[k];
      std::string constraint = params[k]->constraint() ? params[k]->constraint()->name().str() : "";
      for (const Type *t : params[k]->alternatives()) {
        constraint += (constraint.empty() ? "" : " | ") + t->name().str();
      }
      if (decl.constraint && decl.constraint->kind() == TypeRefKind::NAME_REF) {
        constraint = decl.constraint->name.str();
      }
      this->checker.diags_.error(loc, "type '" + args[k]->name() +
                                          "' does not satisfy the constraint '" + constraint +
                                          "' of '" + params[k]->name() + "' in '" + name + "'");
      return nullptr;
    }
  }
  if (emitter && !this->checker.eventMaps().checkEventMap(args[0], loc)) {
    return nullptr;
  }
  auto key = std::make_pair(static_cast<const ClassDecl *>(generic), args);
  if (auto found = this->checker.classInstances_.find(key);
      found != this->checker.classInstances_.end()) {
    if (Checker::Instance *check = this->classChecks.lookup(found->second);
        check && !check->callLoc.isValid()) {
      check->callLoc = use;
    }
    return found->second;
  }
  if (llvm::count_if(this->checker.filling_, [&](const ClassDecl *c) {
        return c->generic() == generic;
      }) >= kMaxClassChain) {
    if (this->endless.insert(generic).second) {
      this->checker.diags_.error(
          generic->loc(), "generic class '" + name +
                              "' uses itself with type arguments that keep "
                              "growing, so each instance of it needs another, without end; a "
                              "generic class can use itself only with its own type parameters");
    }

    return nullptr;
  }

  bool muted = this->checker.diags_.setMuted(true);
  Lexer lexer(generic->source, this->checker.diags_);
  Parser parser(lexer, this->checker.ctx_, this->checker.diags_, generic->library());
  ClassDecl *inst = parser.parseClassCopy();
  this->checker.diags_.setMuted(muted);
  inst->generics = GenericInstance{.generic = generic};
  inst->typeArgs = args;
  inst->isAbstract = generic->isAbstract;
  inst->origin = generic->origin;
  inst->checkOnly = llvm::any_of(args, [](const Type *t) { return mentionsParam(t); });
  inst->stage = ClassStage::RESOLVED;
  inst->declared = true;
  inst->id = ++this->checker.classCount_;
  size_t shown = args.size();
  while (shown > 0 && shown - 1 < defaults.size() && defaults[shown - 1] &&
         (defaults[shown - 1] == args[shown - 1] ||
          (defaults[shown - 1]->isNever() && args[shown - 1]->isNever()))) {
    --shown;
  }
  std::string display =
      shown == 0
          ? name
          : name + "<" +
                GenericClassInstantiator::typeNames(llvm::ArrayRef(args).take_front(shown)) + ">";
  Type *type = Type::classType(display, inst);
  type->setTypeArgs(args);
  inst->type = type;
  this->checker.classes_.push_back(inst);
  generic->generics.as<GenericTemplate>()->instances.push_back(inst);
  this->checker.classInstances_[key] = inst;
  if (Module *home = this->checker.declModules_.lookup(generic)) {
    this->checker.declModules_[inst] = home;
  }

  Checker::Instance *check = nullptr;
  if (!inst->checkOnly) {
    check = &this->checker.instanceStore_.emplace_back();
    check->cls = inst;
    check->callLoc = use;
    check->requester = this->checker.checkingInstance_;
    this->classChecks[inst] = check;
  }
  if (!this->checker.methodsDeclared_) {
    this->checker.unfilled_.push_back(inst);
  } else {
    this->checker.filling_.push_back(inst);
    this->fillInstance(inst);
    this->checker.filling_.pop_back();
    if (this->checker.classesLinked_) {
      this->linkInstance(inst);
    }
  }
  if (check) {
    if (this->checker.checkingInstance_) {
      this->checker.genericClassChecker().checkClassInstance(check);
      if (check->failed) {
        this->checker.genericClassChecker().reportClassInstance(check, loc);
      }
    } else {
      this->checker.pendingInstances_.push_back(check);
    }
  }

  return inst;
}

void GenericClassInstantiator::declareInstanceMember(ClassDecl *inst, FunctionDecl *fn,
                                                     FunctionDecl *generic) {
  FuncSymbol *of = generic->sym;
  FuncSymbol *sym = this->checker.ctx_.newFunc();
  sym->name = fn->name;
  sym->loc = fn->loc();
  sym->decl = fn;
  fn->sym = sym;
  fn->generic = generic;
  if (ClassMember *member = fn->member()) {
    member->variant = generic->variant();
  }
  if (Overriding *overriding = fn->overriding()) {
    overriding->overridden = generic->overridden();
    overriding->openToLate = generic->openToLate();
  }
  if (!of) {
    return;
  }
  auto sub = [&](const Type *t) {
    return this->checker.genericClassChecker().instanceType(inst, t);
  };
  std::vector<const Type *> own = of->typeParams;
  std::vector<const Type *> mine = of->typeParams;
  for (const Type *&p : mine) {
    if (p->iterated() && typeMentions(p->iterated(), inst->generic()->typeArgs)) {
      Type *param = Type::param("Iterable<" + sub(p->iterated())->name().str() + ">");
      param->setIterated(sub(p->iterated()));
      p = param;
    }
  }
  auto sub2 = [&](const Type *t) {
    return t ? this->checker.typeParams().substitute(sub(t), own, mine) : nullptr;
  };
  auto value = [&](const Type *t) {
    return t ? this->checker.typeParams().substituteValue(
                   this->checker.genericClassChecker().instanceValueType(inst, t), own, mine)
             : nullptr;
  };
  fn->typeArgs = mine;
  sym->typeParams = mine;
  for (const Type *d : of->typeDefaults) {
    sym->typeDefaults.push_back(sub2(d));
  }
  for (const Type *p : of->params) {
    sym->params.push_back(value(p));
  }
  sym->pendingParams = of->pendingParams;
  for (size_t i = 0; i < fn->params.size() && i < generic->params.size(); ++i) {
    fn->params[i].declaredType = value(generic->params[i].declaredType);
  }
  for (size_t i = fn->params.size(); i < generic->params.size(); ++i) {
    fn->params.push_back(generic->params[i]);
    fn->params.back().declaredType = value(generic->params[i].declaredType);
  }
  for (size_t i = fn->typeParams.size(); i < generic->typeParams.size(); ++i) {
    fn->typeParams.push_back(generic->typeParams[i]);
  }
  sym->minArgs = of->minArgs;
  sym->hasRest = of->hasRest;
  sym->ret = sub2(of->ret);
  sym->discardsResult = of->discardsResult;
  if (of->predicate) {
    const Type *type = sub2(of->predicate->type);
    sym->predicate = TypePredicate{.param = of->predicate->param, .type = type};
  }
  if (Module *home = this->checker.declModules_.lookup(generic)) {
    this->checker.declModules_[fn] = home;
  }
}

void GenericClassInstantiator::fillInstance(ClassDecl *inst) {
  ClassDecl *generic = inst->generic();
  auto sub = [&](const Type *t) {
    return this->checker.genericClassChecker().instanceType(inst, t);
  };
  auto addInstanceBase = [&](const Type *base) {
    this->checker.classBases().addBase(inst->type, base);
    if (inst->isEmitted() && !llvm::is_contained(this->checker.file_->subtypes, inst->type)) {
      this->checker.file_->subtypes.push_back(inst->type);
    }
  };
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
  this->checker.chunkTypes().addStreamBases(inst, inst->loc());
  for (size_t i = 0; i < inst->fields.size() && i < generic->fields.size(); ++i) {
    ClassField &f = inst->fields[i];
    const ClassField &g = generic->fields[i];
    f.presence = g.presence;
    f.redeclared = g.redeclared;
    if (f.isStatic()) {
      continue;
    }
    f.resolved = g.method()
                     ? sub(g.resolved)
                     : this->checker.genericClassChecker().instanceValueType(inst, g.resolved);
    if (g.method()) {
      FunctionDecl *fn = cast<FunctionExpr>(f.init)->fn;
      fn->name = g.method()->name;
      fn->role = ClassMember{
          .cls = inst, .access = g.method()->access(), .isOverride = g.method()->isOverride()};
      fn->place = FunctionPlace::DECLARED;
      f.form = ArrowMethodField{.method = fn};
    }
  }
  for (size_t i = 0; i < inst->methods.size() && i < generic->methods.size(); ++i) {
    if (!generic->methods[i]->isStatic()) {
      this->declareInstanceMember(inst, inst->methods[i], generic->methods[i]);
    }
  }
  for (size_t i = 0; i < inst->constructors.size() && i < generic->constructors.size(); ++i) {
    this->declareInstanceMember(inst, inst->constructors[i], generic->constructors[i]);
  }
  for (size_t i = 0; i < inst->fields.size() && i < generic->fields.size(); ++i) {
    if (inst->fields[i].method() && generic->fields[i].method()) {
      this->declareInstanceMember(inst, inst->fields[i].method(), generic->fields[i].method());
    }
  }
  if (generic->init) {
    std::vector<Stmt *> sets;
    for (const ClassField &f : inst->fields) {
      if (f.isStatic() || !f.init || f.method()) {
        continue;
      }
      auto *target = this->checker.ctx_.make<MemberExpr>(
          f.loc, this->checker.ctx_.make<NameExpr>(f.loc, "this"), f.name);
      auto *set = this->checker.ctx_.make<AssignExpr>(f.loc, std::nullopt, target, f.init);
      sets.push_back(this->checker.ctx_.make<ExprStmt>(f.loc, set));
    }
    auto *init = this->checker.ctx_.make<FunctionDecl>(inst->loc(), "init");
    init->role = ClassMember{.cls = inst};
    init->body = this->checker.ctx_.make<BlockStmt>(inst->loc());
    init->body->body = std::move(sets);
    this->declareInstanceMember(inst, init, generic->init);
    init->sym->ret = Type::voidTy();
    inst->init = init;
  }
  std::vector<Diagnostics::Captured> errors;
  std::vector<Diagnostics::Captured> *outer = this->checker.diags_.setCapture(&errors);
  this->checker.classDeclarer().checkVariants(inst);
  this->checker.diags_.setCapture(outer);
  if (!errors.empty()) {
    if (Checker::Instance *check = this->classChecks.lookup(inst); check && !check->failed) {
      check->failed = true;
      check->message = errors.front().message;
      check->errorLoc = errors.front().loc;
    }
  }
}

void GenericClassInstantiator::linkInstance(ClassDecl *inst) {
  bool muted = this->checker.diags_.setMuted(true);
  this->checker.classLinks().linkOverrides(inst);
  this->checker.diags_.setMuted(muted);
  this->checker.classLinks().buildVtable(inst);
}

void GenericClassInstantiator::markOverridden(FunctionDecl *fn) {
  if (Overriding *overriding = fn->overriding()) {
    overriding->overridden = true;
  }
  FunctionDecl *generic = fn->generic ? fn->generic : fn;
  ClassDecl *cls = generic->cls();
  if (!cls || !cls->isGeneric()) {
    return;
  }
  if (Overriding *overriding = generic->overriding()) {
    overriding->overridden = true;
  }
  for (ClassDecl *inst : cls->instances()) {
    for (FunctionDecl *m : ClassLinks::ownMethods(inst)) {
      Overriding *overriding = m->overriding();
      if (m->generic == generic && overriding) {
        overriding->overridden = true;
      }
    }
  }
}

}
