#include "compiler/semantic/semantic_state/event_store/event_store.h"

namespace tz {

EventSite *EventStore::findSite(const std::string &key) {
  auto found = this->sites.find(key);

  return found == this->sites.end() ? nullptr : &found->second;
}

EventSite *EventStore::addSite(EventSite site) {
  std::string key = site.key;
  const Type *map = site.map;
  auto placed = this->sites.emplace(key, std::move(site)).first;
  this->mapSites[map] = key;

  return &placed->second;
}

EventSite *EventStore::findSiteOfMap(const Type *map) {
  auto found = this->mapSites.find(map);

  return found == this->mapSites.end() ? nullptr : this->findSite(found->second);
}

std::vector<EventSite *> EventStore::getSites() {
  std::vector<EventSite *> all;
  for (auto &entry : this->sites) {
    all.push_back(&entry.second);
  }

  return all;
}

void EventStore::markSentEmitsFinished() {
  this->sentEmitsFinished = true;
}

bool EventStore::hasFinishedSentEmits() const {
  return this->sentEmitsFinished;
}

bool EventStore::hasCandidates(const Module *module) const {
  return this->candidates.count(module) > 0;
}

void EventStore::rememberCandidates(const Module *module, std::vector<EmitCandidate> found) {
  this->candidates[module] = std::move(found);
}

const std::vector<EmitCandidate> &EventStore::getCandidates(const Module *module) const {
  return this->candidates.at(module);
}

void EventStore::addErrorEmit(const Type *events, llvm::SMLoc loc) {
  for (const ErrorEmit &known : this->errorEmits) {
    if (known.loc == loc) {
      return;
    }
  }
  this->errorEmits.push_back({events, loc});
}

std::vector<ErrorEmit> EventStore::getErrorEmits() const {
  return this->errorEmits;
}

void EventStore::markErrorListened(const Type *events) {
  this->errorListenedMaps.insert(events);
}

bool EventStore::isErrorListened(const Type *events) const {
  return this->errorListenedMaps.contains(events);
}

void EventStore::addSelfListener(const ClassDecl *cls) {
  this->selfListeners.insert(cls);
}

bool EventStore::isSelfListener(const ClassDecl *cls) const {
  return this->selfListeners.contains(cls);
}

void EventStore::addDroppedNew(const ClassDecl *cls, llvm::SMLoc loc) {
  for (const DroppedNew &known : this->droppedNews) {
    if (known.loc == loc) {
      return;
    }
  }
  this->droppedNews.push_back({cls, loc});
}

std::vector<DroppedNew> EventStore::getDroppedNews() const {
  return this->droppedNews;
}

void EventStore::markErrorSubscriptionReported(llvm::SMLoc loc) {
  this->reportedErrorSubscriptions.insert(loc.getPointer());
}

bool EventStore::isErrorSubscriptionReported(llvm::SMLoc loc) const {
  return this->reportedErrorSubscriptions.contains(loc.getPointer());
}

}
