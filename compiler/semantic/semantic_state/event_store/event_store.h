#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Support/SMLoc.h"
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace tz {

struct Module;

enum class ListenerKind { Plain, Async, This };

enum class EventState { COLLECTING, COMPLETING, DONE };

struct SentEmit {
  CallExpr *call = nullptr;
  MemberExpr *member = nullptr;
  ClassDecl *cls = nullptr;
  bool super = false;
};

struct EventRecord {
  std::string name;
  const Type *args = nullptr;
  std::string where;
  std::string untyped;
  EventState state = EventState::COLLECTING;
  std::vector<SentEmit> sent;
};

struct EventSite {
  std::string key;
  Type *map = nullptr;
  Module *owner = nullptr;
  const ClassDecl *cls = nullptr;
  bool complete = false;
  std::deque<EventRecord> events;
};

struct EmitCandidate {
  const CallExpr *call = nullptr;
  Module *module = nullptr;
  FunctionDecl *function = nullptr;
  size_t index = 0;
  std::string event;
  bool self = false;
};

struct ErrorEmit {
  const Type *events = nullptr;
  llvm::SMLoc loc;
};

struct DroppedNew {
  const ClassDecl *cls = nullptr;
  llvm::SMLoc loc;
};

class EventStore {
public:
  EventSite *findSite(const std::string &key);
  EventSite *addSite(EventSite site);
  EventSite *findSiteOfMap(const Type *map);
  std::vector<EventSite *> getSites();
  void markSentEmitsFinished();
  bool hasFinishedSentEmits() const;
  bool hasCandidates(const Module *module) const;
  void rememberCandidates(const Module *module, std::vector<EmitCandidate> found);
  const std::vector<EmitCandidate> &getCandidates(const Module *module) const;
  void addErrorEmit(const Type *events, llvm::SMLoc loc);
  std::vector<ErrorEmit> getErrorEmits() const;
  void markErrorListened(const Type *events);
  bool isErrorListened(const Type *events) const;
  void addSelfListener(const ClassDecl *classDecl);
  bool isSelfListener(const ClassDecl *classDecl) const;
  void addDroppedNew(const ClassDecl *classDecl, llvm::SMLoc loc);
  std::vector<DroppedNew> getDroppedNews() const;
  void markErrorSubscriptionReported(llvm::SMLoc loc);
  bool isErrorSubscriptionReported(llvm::SMLoc loc) const;

private:
  std::map<std::string, EventSite> sites;
  llvm::DenseMap<const Type *, std::string> mapSites;
  std::map<const Module *, std::vector<EmitCandidate>> candidates;
  bool sentEmitsFinished = false;
  std::vector<ErrorEmit> errorEmits;
  llvm::SmallPtrSet<const Type *, 4> errorListenedMaps;
  llvm::SmallPtrSet<const ClassDecl *, 4> selfListeners;
  std::vector<DroppedNew> droppedNews;
  llvm::DenseSet<const void *> reportedErrorSubscriptions;
};

}
