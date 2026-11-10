#include "compiler/semantic/generics/generic_class_instantiator/generic_class_instantiator.h"
#include "compiler/program/ast/ast_cloner/ast_cloner.h"
#include "compiler/semantic/bodies/classes/class_bases/class_bases.h"
#include "compiler/semantic/bodies/classes/class_links/class_links.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/generics/generic_check_helpers/generic_check_helpers.h"
#include "compiler/semantic/generics/generic_class_checker/generic_class_checker.h"
#include "compiler/semantic/generics/inference/inference.h"
#include "compiler/semantic/generics/type_params/type_params.h"
#include "compiler/semantic/library/events/event_maps/event_maps.h"
#include "compiler/semantic/library/stream/chunk_types/chunk_types.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;

namespace tz {

namespace {

constexpr unsigned kMaxClassChain = 16;

}

GenericClassInstantiator::GenericClassInstantiator(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->chunkTypes = &all.getChunkTypes();
  this->classBases = &all.getClassBases();
  this->classLinks = &all.getClassLinks();
  this->typeParams = &all.getTypeParams();
  this->classDeclarer = &all.getClassDeclarer();
  this->genericClassChecker = &all.getGenericClassChecker();
}

std::string GenericClassInstantiator::typeNames(llvm::ArrayRef<const Type *> types) {
  std::string text;
  for (const Type *t : types) {
    text += (text.empty() ? "" : ", ") + t->name().str();
  }

  return text;
}

GenericClassInstantiator::ResultClassMade
GenericClassInstantiator::instantiateClass(ClassDecl *generic, std::vector<const Type *> args,
                                           llvm::SMLoc use) {
  this->genericClassChecker->declareClassTypeParams(generic);
  llvm::SMLoc loc = use.isValid() ? use : generic->loc();
  std::vector<const Type *> params = generic->typeArgs;
  std::vector<const Type *> defaults = this->state.getGenericStore().getClassDefaults(generic);
  std::string name = generic->name.str();
  if (args.size() > params.size()) {
    std::string message = ErrorTextClassInstance::tooMany(name, params.size(), args.size());

    return err(GenericClassInstantiatorInstantiateClassError{
        .code = GenericClassInstantiatorInstantiateClassErrorCode::TOO_MANY_TYPE_ARGUMENTS,
        .message = message,
    });
  }
  bool emitter = generic->library() && generic->name == "EventEmitter";
  for (size_t k = args.size(); k < params.size(); ++k) {
    if (emitter) {
      std::string message = ErrorTextClassInstance::eventMap();

      return err(GenericClassInstantiatorInstantiateClassError{
          .code = GenericClassInstantiatorInstantiateClassErrorCode::EVENT_MAP_MISSING,
          .message = message,
      });
    }
    if (k >= defaults.size() || !defaults[k]) {
      std::string message = ErrorTextClassInstance::needs(name, params.size());

      return err(GenericClassInstantiatorInstantiateClassError{
          .code = GenericClassInstantiatorInstantiateClassErrorCode::TYPE_ARGUMENTS_MISSING,
          .message = message,
      });
    }
    std::vector<const Type *> known = args;
    known.resize(params.size(), nullptr);
    args.push_back(this->typeParams->substitute(defaults[k], params, known));
  }
  for (size_t k = 0; k < params.size(); ++k) {
    if (args[k]->isError()) {
      return err(GenericClassInstantiatorInstantiateClassError{
          .code = GenericClassInstantiatorInstantiateClassErrorCode::ALREADY_REPORTED,
          .message = "",
      });
    }
    if (args[k]->isUndefined()) {
      std::string message = ErrorTextClassInstance::undefinedArg(args[k]->name(), name);

      return err(GenericClassInstantiatorInstantiateClassError{
          .code = GenericClassInstantiatorInstantiateClassErrorCode::UNDEFINED_TYPE_ARGUMENT,
          .message = message,
      });
    }
    if (!this->typeParams->satisfies(args[k], params[k], params, args)) {
      const TypeParamDecl &decl = generic->typeParams[k];
      std::string constraint = params[k]->constraint() ? params[k]->constraint()->name().str() : "";
      for (const Type *t : params[k]->alternatives()) {
        constraint += (constraint.empty() ? "" : " | ") + t->name().str();
      }
      if (decl.constraint && decl.constraint->kind() == TypeRefKind::NAME_REF) {
        constraint = decl.constraint->name.str();
      }
      std::string message =
          ErrorTextClassInstance::unsatisfied(args[k]->name(), constraint, params[k]->name(), name);

      return err(GenericClassInstantiatorInstantiateClassError{
          .code = GenericClassInstantiatorInstantiateClassErrorCode::CONSTRAINT_UNSATISFIED,
          .message = message,
      });
    }
  }
  if (emitter) {
    EventMaps::ResultEventMapFits fits = EventMaps::eventMapFits(args[0]);
    if (fits.isErr()) {
      this->state.getDiagnostics().error(loc, fits.error().message);

      return err(GenericClassInstantiatorInstantiateClassError{
          .code = GenericClassInstantiatorInstantiateClassErrorCode::ALREADY_REPORTED,
          .message = "",
      });
    }
  }
  GenericStore &generics = this->state.getGenericStore();
  if (ClassDecl *known = generics.findClassInstance(generic, args)) {
    if (InstanceRecord *check = generics.findClassCheck(known);
        check && !check->callLoc.isValid()) {
      check->callLoc = use;
    }
    return ok(known);
  }
  if (generics.countFillingOf(generic) >= int(kMaxClassChain)) {
    bool first = !generics.isEndlessReported(generic);
    generics.markEndlessReported(generic);
    if (first) {
      std::string message = ErrorTextClassInstance::endless(name);

      return err(GenericClassInstantiatorInstantiateClassError{
          .code = GenericClassInstantiatorInstantiateClassErrorCode::ENDLESS_INSTANCES,
          .message = message,
      });
    }

    return err(GenericClassInstantiatorInstantiateClassError{
        .code = GenericClassInstantiatorInstantiateClassErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }

  AstCloner cloner(this->state.getAst());
  ClassDecl *inst = cloner.copyClass(generic);
  inst->decorators.clear();
  inst->syntax = StatementClass{};
  inst->generics = GenericInstance{.generic = generic};
  inst->typeArgs = args;
  inst->isAbstract = generic->isAbstract;
  inst->origin = generic->origin;
  inst->checkOnly = llvm::any_of(args, [](const Type *t) { return TypeQueries::mentionsParam(t); });
  inst->stage = ClassStage::RESOLVED;
  inst->declared = true;
  inst->id = this->state.getClassStore().takeClassId();
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
  this->state.getClassStore().addClass(inst);
  generic->generics.as<GenericTemplate>()->instances.push_back(inst);
  generics.rememberClassInstance(generic, args, inst);
  if (Module *home = this->state.getModuleStates().findDeclModule(generic)) {
    this->state.getModuleStates().rememberDeclModule(inst, home);
  }

  InstanceRecord *check = nullptr;
  if (!inst->checkOnly) {
    check = generics.addInstanceRecord();
    check->cls = inst;
    check->callLoc = use;
    check->requester = this->state.getGenericStore().getCheckingInstance();
    generics.rememberClassCheck(inst, check);
  }
  if (!this->state.getClassStore().hasDeclaredMethods()) {
    generics.addUnfilledClass(inst);
  } else {
    generics.enterFilling(inst);
    this->fillInstance(inst);
    generics.leaveFilling();
    if (this->state.getClassStore().hasLinkedClasses()) {
      this->linkInstance(inst);
    }
  }
  if (check) {
    if (this->state.getGenericStore().isCheckingInstance()) {
      this->genericClassChecker->checkClassInstance(check);
      if (check->failed) {
        this->genericClassChecker->reportClassInstance(check, loc);
      }
    } else {
      generics.addPendingInstance(check);
    }
  }

  return ok(inst);
}

void GenericClassInstantiator::declareInstanceMember(ClassDecl *inst, FunctionDecl *fn,
                                                     FunctionDecl *generic) {
  FuncSymbol *of = generic->sym;
  FuncSymbol *sym = this->state.getAst().newFunc();
  sym->setCompleter(this->state.getCompleter());
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
  auto sub = [&](const Type *t) { return this->genericClassChecker->instanceType(inst, t); };
  std::vector<const Type *> own = of->typeParams;
  std::vector<const Type *> mine = of->typeParams;
  for (const Type *&p : mine) {
    if (p->iterated() &&
        GenericCheckHelpers::mentionsAnyOf(p->iterated(), inst->generic()->typeArgs)) {
      Type *param = Type::param("Iterable<" + sub(p->iterated())->name().str() + ">");
      param->setIterated(sub(p->iterated()));
      p = param;
    }
  }
  auto sub2 = [&](const Type *t) {
    return t ? this->typeParams->substitute(sub(t), own, mine) : nullptr;
  };
  auto value = [&](const Type *t) {
    return t ? this->typeParams->substituteValue(
                   this->genericClassChecker->instanceValueType(inst, t), own, mine)
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
  sym->setReturnType(sub2(of->getKnownReturnType()));
  sym->discardsResult = of->discardsResult;
  if (of->predicate) {
    const Type *type = sub2(of->predicate->type);
    sym->predicate = TypePredicate{.param = of->predicate->param, .type = type};
  }
  if (Module *home = this->state.getModuleStates().findDeclModule(generic)) {
    this->state.getModuleStates().rememberDeclModule(fn, home);
  }
}

void GenericClassInstantiator::fillInstance(ClassDecl *inst) {
  ClassDecl *generic = inst->generic();
  auto sub = [&](const Type *t) { return this->genericClassChecker->instanceType(inst, t); };
  auto addInstanceBase = [&](const Type *base) {
    this->classBases->addBase(inst->type, base);
    if (inst->isEmitted()) {
      this->state.getInventoryStore().addSubtype(inst->type);
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
  this->chunkTypes->addStreamBases(inst, inst->loc());
  for (size_t i = 0; i < inst->fields.size() && i < generic->fields.size(); ++i) {
    ClassField &f = inst->fields[i];
    const ClassField &g = generic->fields[i];
    f.presence = g.presence;
    f.redeclared = g.redeclared;
    if (f.isStatic()) {
      continue;
    }
    f.resolved = g.method() ? sub(g.resolved)
                            : this->genericClassChecker->instanceValueType(inst, g.resolved);
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
      auto *target = this->state.getAst().make<MemberExpr>(
          f.loc, this->state.getAst().make<NameExpr>(f.loc, "this"), f.name);
      auto *set = this->state.getAst().make<AssignExpr>(f.loc, std::nullopt, target, f.init);
      sets.push_back(this->state.getAst().make<ExprStmt>(f.loc, set));
    }
    auto *init = this->state.getAst().make<FunctionDecl>(inst->loc(), "init");
    init->role = ClassMember{.cls = inst};
    init->body = this->state.getAst().make<BlockStmt>(inst->loc());
    init->body->body = std::move(sets);
    this->declareInstanceMember(inst, init, generic->init);
    init->sym->setReturnType(Type::voidTy());
    inst->init = init;
  }
  std::vector<Diagnostics::Captured> errors;
  std::vector<Diagnostics::Captured> *outer = this->state.getDiagnostics().setCapture(&errors);
  this->classDeclarer->checkVariants(inst);
  this->state.getDiagnostics().setCapture(outer);
  if (!errors.empty()) {
    if (InstanceRecord *check = this->state.getGenericStore().findClassCheck(inst);
        check && !check->failed) {
      check->failed = true;
      check->message = errors.front().message;
      check->errorLoc = errors.front().loc;
    }
  }
}

void GenericClassInstantiator::linkInstance(ClassDecl *inst) {
  bool muted = this->state.getDiagnostics().setMuted(true);
  this->classLinks->linkOverrides(inst);
  this->state.getDiagnostics().setMuted(muted);
  this->classLinks->buildVtable(inst);
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
