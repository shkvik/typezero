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

enum class EventState { COLLECTING, COMPLETING, DONE };

enum class CallReceiver {
  OWN,
  SUPER,
};

struct SentEmit {
  CallExpr *call = nullptr;
  MemberExpr *member = nullptr;
  ClassDecl *cls = nullptr;
  CallReceiver callReceiver = CallReceiver::OWN;
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

class EventContext {
public:
  void markSentEmitsFinished();
  void markErrorListened(const Type *events);
  void addSelfListener(const ClassDecl *classDecl);
  void markErrorSubscriptionReported(llvm::SMLoc loc);
  void addErrorEmit(const Type *events, llvm::SMLoc loc);
  void addDroppedNew(const ClassDecl *classDecl, llvm::SMLoc loc);
  void rememberCandidates(const Module *module, std::vector<EmitCandidate> found);


  bool hasFinishedSentEmits() const;
  bool hasCandidates(const Module *module) const;
  bool isErrorListened(const Type *events) const;
  bool isSelfListener(const ClassDecl *classDecl) const;
  bool isErrorSubscriptionReported(llvm::SMLoc loc) const;


  EventSite *addSite(EventSite site);
  EventSite *findSiteOfMap(const Type *map);
  EventSite *findSite(const std::string &key);


  std::vector<EventSite *> getSites();
  std::vector<ErrorEmit> getErrorEmits() const;
  std::vector<DroppedNew> getDroppedNews() const;
  const std::vector<EmitCandidate> &getCandidates(const Module *module) const;

private:
  bool sentEmitsFinished = false;
  std::vector<ErrorEmit> errorEmits;
  std::vector<DroppedNew> droppedNews;
  std::map<std::string, EventSite> sites;
  llvm::DenseMap<const Type *, std::string> mapSites;
  llvm::SmallPtrSet<const Type *, 4> errorListenedMaps;
  llvm::SmallPtrSet<const ClassDecl *, 4> selfListeners;
  llvm::DenseSet<const void *> reportedErrorSubscriptions;
  std::map<const Module *, std::vector<EmitCandidate>> candidates;
};

}
