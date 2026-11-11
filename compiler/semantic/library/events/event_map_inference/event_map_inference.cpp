#include "compiler/semantic/library/events/event_map_inference/event_map_inference.h"
#include "compiler/program/ast/ast_cloner/ast_cloner.h"
#include "compiler/program/ast_walker/ast_walker.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/bodies/functions/function_checker/function_checker.h"
#include "compiler/semantic/bodies/stmt/stmt_checker/stmt_checker.h"
#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/Support/Path.h"

using llvm::dyn_cast;

namespace tz {

EventMapInference::EventMapInference(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.scopes;
  this->stmtChecker = &all.stmtChecker;
  this->returnInferrer = &all.returnInferrer;
  this->functionChecker = &all.functionChecker;
}

EventRecord *EventMapInference::findEvent(EventSite *site, llvm::StringRef name) {
  for (EventRecord &record : site->events) {
    if (record.name == name) {
      return &record;
    }
  }

  return nullptr;
}

bool EventMapInference::isFrozen(const EventRecord *record) {
  return record != nullptr && record->state == EventState::DONE && record->args != nullptr;
}

std::string EventMapInference::fileOfPlace(const std::string &place) {
  size_t column = place.rfind(':');
  size_t line =
      column == std::string::npos || column == 0 ? std::string::npos : place.rfind(':', column - 1);

  return line == std::string::npos ? place : place.substr(0, line);
}

std::vector<FunctionDecl *> EventMapInference::classMethods(const ClassDecl *cls) {
  std::vector<FunctionDecl *> methods = cls->methods;
  for (const ClassField &field : cls->fields) {
    if (field.method() != nullptr) {
      methods.push_back(field.method());
    }
  }

  return methods;
}

bool EventMapInference::holdsClassExpr(const Stmt *where) {
  std::vector<const Expr *> exprs = AstWalker::exprsIn(where, true);

  return llvm::any_of(exprs, [](const Expr *expr) { return llvm::isa<ClassExpr>(expr); });
}

bool EventMapInference::canTryFunction(const FunctionDecl *fn) {
  const ClassDecl *cls = fn->cls();
  bool generic = fn->sym != nullptr && fn->sym->isGeneric();
  bool genericClass = cls != nullptr && cls->isGeneric();
  bool usable = fn->body != nullptr && fn->sym != nullptr && !generic && !genericClass;

  return usable && !EventMapInference::holdsClassExpr(fn);
}

void EventMapInference::addCandidates(const Stmt *where, Module *module, FunctionDecl *fn,
                                      size_t index, std::vector<EmitCandidate> &found) {
  for (const Expr *expr : AstWalker::exprsIn(where, true)) {
    auto *call = dyn_cast<CallExpr>(expr);
    auto *member = call != nullptr ? dyn_cast<MemberExpr>(call->callee) : nullptr;
    if (member == nullptr || member->name != "emit" || call->args.empty()) {
      continue;
    }
    auto *name = dyn_cast<StringLitExpr>(call->args[0]);
    if (name == nullptr) {
      continue;
    }
    auto *receiver = dyn_cast<NameExpr>(member->object);
    bool self = receiver != nullptr && (receiver->name == "this" || receiver->name == "super");
    EmitCandidate candidate;
    candidate.call = call;
    candidate.module = module;
    candidate.function = fn;
    candidate.index = index;
    candidate.event = ToUtf8::toUtf8(name->value);
    candidate.self = self;
    found.push_back(std::move(candidate));
  }
}

void EventMapInference::defineMapFields(EventSite *site) {
  std::vector<Field> fields;
  for (const EventRecord &record : site->events) {
    if (record.args != nullptr) {
      fields.push_back({record.name, record.args, false});
    }
  }
  site->map->defineFields(std::move(fields));
}

std::string EventMapInference::placeOf(llvm::SMLoc loc) const {
  llvm::SourceMgr &sm = this->state.getDiagnostics().sourceMgr();
  unsigned id = loc.isValid() ? sm.FindBufferContainingLoc(loc) : 0;
  if (!id) {
    return "?";
  }
  auto [line, column] = sm.getLineAndColumn(loc, id);

  return sm.getMemoryBuffer(id)->getBufferIdentifier().str() + ":" + std::to_string(line) + ":" +
         std::to_string(column);
}

std::string EventMapInference::shortPlaceOf(llvm::SMLoc loc) const {
  std::string place = this->placeOf(loc);
  std::string file = EventMapInference::fileOfPlace(place);
  std::string name = llvm::sys::path::filename(file).str();

  return name + place.substr(file.size());
}

Module *EventMapInference::findModuleAt(llvm::SMLoc loc) const {
  llvm::SourceMgr &sm = this->state.getDiagnostics().sourceMgr();
  unsigned id = loc.isValid() ? sm.FindBufferContainingLoc(loc) : 0;
  if (id == 0) {
    return nullptr;
  }
  for (Module *module : this->state.getInventoryStore().getModules()) {
    if (llvm::is_contained(module->buffers, id)) {
      return module;
    }
  }

  return nullptr;
}

Type *EventMapInference::siteMap(const std::string &key, const std::string &name, Module *owner,
                                 const ClassDecl *cls) {
  EventStore &store = this->state.getEventStore();
  EventSite *known = store.findSite(key);
  if (known != nullptr) {
    return known->map;
  }
  Type *map = Type::declareObject(name);
  map->defineFields({});
  EventSite site;
  site.key = key;
  site.map = map;
  site.owner = owner;
  site.cls = cls;
  EventSite *added = store.addSite(std::move(site));

  return added->map;
}

Type *EventMapInference::classEventMap(const ClassDecl *cls) {
  std::string key = "class " + this->placeOf(cls->loc()) + " " + cls->name.str();
  std::string name = "events of " + cls->name.str();
  Module *declared = this->state.getModuleStates().findDeclModule(cls);
  Module *owner = declared != nullptr ? declared : this->findModuleAt(cls->loc());

  return this->siteMap(key, name, owner, cls);
}

Type *EventMapInference::newEventMap(llvm::SMLoc loc) {
  std::string key = "new " + this->placeOf(loc);
  std::string name = "events at " + this->shortPlaceOf(loc);
  Module *owner = this->findModuleAt(loc);

  return this->siteMap(key, name, owner, nullptr);
}

EventRecord *EventMapInference::addEvent(EventSite *site, llvm::StringRef name) {
  EventRecord record;
  record.name = name.str();
  site->events.push_back(std::move(record));

  return &site->events.back();
}

void EventMapInference::rememberEventArgs(EventSite *site, EventRecord *record, const Type *args,
                                          llvm::SMLoc loc) {
  record->args = args;
  if (record->where.empty()) {
    record->where = this->shortPlaceOf(loc);
  }
  EventMapInference::defineMapFields(site);
}

EventRecord *EventMapInference::completeEvent(EventSite *site, llvm::StringRef name) {
  EventRecord *record = EventMapInference::findEvent(site, name);
  if (record != nullptr && record->state != EventState::COLLECTING) {
    return record;
  }
  if (record == nullptr) {
    record = this->addEvent(site, name);
  }
  record->state = EventState::COMPLETING;
  this->tryCandidates(site, record);
  record->state = EventState::DONE;

  return record;
}

void EventMapInference::completeSite(EventSite *site) {
  if (site->complete) {
    return;
  }
  site->complete = true;
  std::vector<std::string> names;
  for (const EmitCandidate &candidate : this->getSiteCandidates(site)) {
    if (!llvm::is_contained(names, candidate.event)) {
      names.push_back(candidate.event);
    }
  }
  for (const std::string &name : names) {
    this->completeEvent(site, name);
  }
}

const std::vector<EmitCandidate> &EventMapInference::getModuleCandidates(Module *module) {
  EventStore &store = this->state.getEventStore();
  if (store.hasCandidates(module)) {
    return store.getCandidates(module);
  }
  std::vector<EmitCandidate> found;
  std::vector<Stmt *> &items = module->file.items;
  for (size_t i = 0; i < items.size(); ++i) {
    Stmt *item = items[i];
    auto *fn = dyn_cast<FunctionDecl>(item);
    auto *cls = dyn_cast<ClassDecl>(item);
    if (fn != nullptr) {
      EventMapInference::addCandidates(fn, module, fn, i, found);
    } else if (cls != nullptr) {
      for (FunctionDecl *method : EventMapInference::classMethods(cls)) {
        EventMapInference::addCandidates(method, module, method, i, found);
      }
    } else {
      EventMapInference::addCandidates(item, module, nullptr, i, found);
    }
  }
  store.rememberCandidates(module, std::move(found));

  return store.getCandidates(module);
}

std::vector<EmitCandidate> EventMapInference::getSiteCandidates(const EventSite *site) {
  std::vector<EmitCandidate> found;
  for (Module *module : this->state.getInventoryStore().getModules()) {
    bool own = module == site->owner;
    bool skipped = module->builtin || (!own && site->cls == nullptr);
    if (skipped) {
      continue;
    }
    for (const EmitCandidate &candidate : this->getModuleCandidates(module)) {
      if (own || EventMapInference::isSubclassEmit(site, candidate)) {
        found.push_back(candidate);
      }
    }
  }
  std::vector<EmitCandidate> late = this->getLateCandidates();
  for (const EmitCandidate &candidate : late) {
    if (EventMapInference::isSubclassEmit(site, candidate)) {
      found.push_back(candidate);
    }
  }

  return found;
}

std::vector<EmitCandidate> EventMapInference::getLateCandidates() {
  std::vector<EmitCandidate> found;
  for (ClassDecl *cls : this->state.getLateClassStore().getUncheckedLateClasses()) {
    Module *module = this->findModuleAt(cls->loc());
    for (FunctionDecl *method : EventMapInference::classMethods(cls)) {
      EventMapInference::addCandidates(method, module, method, 0, found);
    }
  }

  return found;
}

bool EventMapInference::isSubclassEmit(const EventSite *site, const EmitCandidate &candidate) {
  const ClassDecl *cls = candidate.function != nullptr ? candidate.function->cls() : nullptr;

  return site->cls != nullptr && candidate.self && cls != nullptr && cls->derivesFrom(site->cls);
}

void EventMapInference::tryCandidates(EventSite *site, EventRecord *record) {
  llvm::SmallPtrSet<const FunctionDecl *, 4> functions;
  llvm::SmallSet<size_t, 4> statements;
  std::string untyped;
  for (const EmitCandidate &candidate : this->getSiteCandidates(site)) {
    bool unchecked = candidate.event == record->name && candidate.call->type == nullptr;
    if (!unchecked) {
      continue;
    }
    if (untyped.empty()) {
      untyped = this->shortPlaceOf(candidate.call->loc());
    }
    if (candidate.function != nullptr && functions.insert(candidate.function).second &&
        EventMapInference::canTryFunction(candidate.function)) {
      this->tryFunction(candidate.function);
    }
    if (candidate.function == nullptr && this->canTryStatement(candidate) &&
        statements.insert(candidate.index).second) {
      Stmt *statement = candidate.module->file.items[candidate.index];
      if (!EventMapInference::holdsClassExpr(statement)) {
        this->tryStatement(statement);
      }
    }
  }
  if (record->args == nullptr) {
    record->untyped = untyped;
  }
}

bool EventMapInference::canTryStatement(const EmitCandidate &candidate) {
  ModuleStates &modules = this->state.getModuleStates();
  if (modules.getCurrentModule() != candidate.module) {
    return false;
  }
  ModuleState &here = modules.getModuleState(candidate.module);

  return here.busy && candidate.index >= here.next;
}

void EventMapInference::tryFunction(FunctionDecl *fn) {
  BodyState saved = this->returnInferrer->enterBody(fn);
  bool muted = this->state.getDiagnostics().setMuted(true);
  FunctionStack &functions = this->state.getFunctionStack();
  std::vector<ReturnStmt *> returns;
  std::vector<YieldExpr *> yields;
  functions.rememberReturnCollector(&returns);
  functions.rememberYieldCollector(&yields);
  functions.enterTrial();
  FunctionDecl *copy = this->returnInferrer->freshCopy(fn);
  this->functionChecker->walkFunctionFromScratch(copy);
  functions.leaveTrial();
  this->state.getDiagnostics().setMuted(muted);
  this->returnInferrer->leaveBody(saved);
}

void EventMapInference::tryStatement(Stmt *statement) {
  BodyState saved = this->returnInferrer->enterBody(nullptr);
  bool muted = this->state.getDiagnostics().setMuted(true);
  FunctionStack &functions = this->state.getFunctionStack();
  functions.enterTrial();
  AstCloner cloner(this->state.getAst());
  Stmt *copy = cloner.copyStatement(statement);
  this->scopes->openScope();
  this->stmtChecker->checkStmt(copy);
  this->scopes->closeScope();
  functions.leaveTrial();
  this->state.getDiagnostics().setMuted(muted);
  this->returnInferrer->leaveBody(saved);
}

}
