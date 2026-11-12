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

EventRecord *EventMapInference::findEvent(EventSite *site, llvm::StringRef name) const {
  for (EventRecord &record : site->events) {
    if (record.name == name) {
      return &record;
    }
  }

  return nullptr;
}

bool EventMapInference::isFrozen(const EventRecord *record) const {
  return record != nullptr && record->state == EventState::DONE && record->args != nullptr;
}

std::string EventMapInference::fileOfPlace(const std::string &place) const {
  size_t column = place.rfind(':');
  size_t line =
      column == std::string::npos || column == 0 ? std::string::npos : place.rfind(':', column - 1);

  return line == std::string::npos ? place : place.substr(0, line);
}

std::vector<FunctionDecl *> EventMapInference::classMethods(const ClassDecl *classDecl) const {
  std::vector<FunctionDecl *> methods = classDecl->methods;
  for (const ClassField &field : classDecl->fields) {
    if (field.method() != nullptr) {
      methods.push_back(field.method());
    }
  }

  return methods;
}

bool EventMapInference::holdsClassExpr(const Stmt *where) const {
  auto exprs = AstWalker::exprsIn(where, true);

  return llvm::any_of(exprs, [](const Expr *expr) { return llvm::isa<ClassExpr>(expr); });
}

bool EventMapInference::canTryFunction(const FunctionDecl *functionDecl) const {
  const ClassDecl *classDecl = functionDecl->cls();
  bool isGeneric = functionDecl->sym != nullptr && functionDecl->sym->isGeneric();
  bool isGenericClass = classDecl != nullptr && classDecl->isGeneric();
  bool isUsable = functionDecl->body != nullptr && functionDecl->sym != nullptr && !isGeneric &&
                  !isGenericClass;

  return isUsable && !this->holdsClassExpr(functionDecl);
}

void EventMapInference::addCandidates(const Stmt *where, Module *module, FunctionDecl *functionDecl,
                                      size_t index, std::vector<EmitCandidate> &found) const {
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
    bool isSelf = receiver != nullptr && (receiver->name == "this" || receiver->name == "super");
    EmitCandidate candidate;
    candidate.call = call;
    candidate.module = module;
    candidate.function = functionDecl;
    candidate.index = index;
    candidate.event = ToUtf8::toUtf8(name->value);
    candidate.self = isSelf;
    found.push_back(std::move(candidate));
  }
}

void EventMapInference::defineMapFields(EventSite *site) const {
  auto fields = std::vector<Field>();
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
  auto place = this->placeOf(loc);
  auto file = this->fileOfPlace(place);
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
                                 const ClassDecl *classDecl) {
  auto &store = this->state.getEventStore();
  auto *known = store.findSite(key);
  if (known != nullptr) {
    return known->map;
  }
  auto *map = Type::declareObject(name);
  map->defineFields({});
  EventSite site;
  site.key = key;
  site.map = map;
  site.owner = owner;
  site.cls = classDecl;
  auto *added = store.addSite(std::move(site));

  return added->map;
}

Type *EventMapInference::classEventMap(const ClassDecl *classDecl) {
  std::string key = "class " + this->placeOf(classDecl->loc()) + " " + classDecl->name.str();
  std::string name = "events of " + classDecl->name.str();
  auto *declared = this->state.getModuleStates().findDeclModule(classDecl);
  Module *owner = declared != nullptr ? declared : this->findModuleAt(classDecl->loc());

  return this->siteMap(key, name, owner, classDecl);
}

Type *EventMapInference::newEventMap(llvm::SMLoc loc) {
  std::string key = "new " + this->placeOf(loc);
  std::string name = "events at " + this->shortPlaceOf(loc);
  auto *owner = this->findModuleAt(loc);

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
  this->defineMapFields(site);
}

EventRecord *EventMapInference::completeEvent(EventSite *site, llvm::StringRef name) {
  auto *record = this->findEvent(site, name);
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
  auto names = std::vector<std::string>();
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
  auto &store = this->state.getEventStore();
  if (store.hasCandidates(module)) {
    return store.getCandidates(module);
  }
  auto found = std::vector<EmitCandidate>();
  std::vector<Stmt *> &items = module->file.items;
  for (size_t index = 0; index < items.size(); ++index) {
    Stmt *item = items[index];
    auto *functionDecl = dyn_cast<FunctionDecl>(item);
    auto *classDecl = dyn_cast<ClassDecl>(item);
    if (functionDecl != nullptr) {
      this->addCandidates(functionDecl, module, functionDecl, index, found);
    } else if (classDecl != nullptr) {
      for (FunctionDecl *method : this->classMethods(classDecl)) {
        this->addCandidates(method, module, method, index, found);
      }
    } else {
      this->addCandidates(item, module, nullptr, index, found);
    }
  }
  store.rememberCandidates(module, std::move(found));

  return store.getCandidates(module);
}

std::vector<EmitCandidate> EventMapInference::getSiteCandidates(const EventSite *site) {
  auto found = std::vector<EmitCandidate>();
  for (Module *module : this->state.getInventoryStore().getModules()) {
    bool isOwn = module == site->owner;
    bool isSkipped = module->builtin || (!isOwn && site->cls == nullptr);
    if (isSkipped) {
      continue;
    }
    for (const EmitCandidate &candidate : this->getModuleCandidates(module)) {
      if (isOwn || this->isSubclassEmit(site, candidate)) {
        found.push_back(candidate);
      }
    }
  }
  auto late = this->getLateCandidates();
  for (const EmitCandidate &candidate : late) {
    if (this->isSubclassEmit(site, candidate)) {
      found.push_back(candidate);
    }
  }

  return found;
}

std::vector<EmitCandidate> EventMapInference::getLateCandidates() {
  auto found = std::vector<EmitCandidate>();
  for (ClassDecl *classDecl : this->state.getLateClassStore().getUncheckedLateClasses()) {
    auto *module = this->findModuleAt(classDecl->loc());
    for (FunctionDecl *method : this->classMethods(classDecl)) {
      this->addCandidates(method, module, method, 0, found);
    }
  }

  return found;
}

bool EventMapInference::isSubclassEmit(const EventSite *site,
                                       const EmitCandidate &candidate) const {
  const ClassDecl *classDecl = candidate.function != nullptr ? candidate.function->cls() : nullptr;

  return site->cls != nullptr && candidate.self && classDecl != nullptr &&
         classDecl->derivesFrom(site->cls);
}

void EventMapInference::tryCandidates(EventSite *site, EventRecord *record) {
  auto functions = llvm::SmallPtrSet<const FunctionDecl *, 4>();
  llvm::SmallSet<size_t, 4> statements;
  auto untyped = std::string();
  for (const EmitCandidate &candidate : this->getSiteCandidates(site)) {
    bool isUnchecked = candidate.event == record->name && candidate.call->type == nullptr;
    if (!isUnchecked) {
      continue;
    }
    if (untyped.empty()) {
      untyped = this->shortPlaceOf(candidate.call->loc());
    }
    if (candidate.function != nullptr) {
      auto [_, isNewFunction] = functions.insert(candidate.function);
      if (isNewFunction && this->canTryFunction(candidate.function)) {
        this->tryFunction(candidate.function);
      }
    }
    if (candidate.function == nullptr && this->canTryStatement(candidate)) {
      auto [_, isNewStatement] = statements.insert(candidate.index);
      Stmt *statement = candidate.module->file.items[candidate.index];
      if (isNewStatement && !this->holdsClassExpr(statement)) {
        this->tryStatement(statement);
      }
    }
  }
  if (record->args == nullptr) {
    record->untyped = untyped;
  }
}

bool EventMapInference::canTryStatement(const EmitCandidate &candidate) {
  auto &modules = this->state.getModuleStates();
  if (modules.getCurrentModule() != candidate.module) {
    return false;
  }
  auto &here = modules.getModuleState(candidate.module);

  return here.busy && candidate.index >= here.next;
}

void EventMapInference::tryFunction(FunctionDecl *functionDecl) {
  auto saved = this->returnInferrer->enterBody(functionDecl);
  this->state.getDiagnostics().pushList(DiagnosticRoute::DISCARD);
  auto &functions = this->state.getFunctionStack();
  auto returns = std::vector<ReturnStmt *>();
  auto yields = std::vector<YieldExpr *>();
  functions.rememberReturnCollector(&returns);
  functions.rememberYieldCollector(&yields);
  functions.enterTrial();
  auto *copy = this->returnInferrer->freshCopy(functionDecl);
  this->functionChecker->walkFunctionFromScratch(copy);
  functions.leaveTrial();
  this->state.getDiagnostics().popList();
  this->returnInferrer->leaveBody(saved);
}

void EventMapInference::tryStatement(Stmt *statement) {
  auto saved = this->returnInferrer->enterBody(nullptr);
  this->state.getDiagnostics().pushList(DiagnosticRoute::DISCARD);
  auto &functions = this->state.getFunctionStack();
  functions.enterTrial();
  AstCloner cloner(this->state.getAst());
  Stmt *copy = cloner.copyStatement(statement);
  this->scopes->openScope();
  this->stmtChecker->walkStmt(copy);
  this->scopes->closeScope();
  functions.leaveTrial();
  this->state.getDiagnostics().popList();
  this->returnInferrer->leaveBody(saved);
}

}
