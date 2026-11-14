#include "compiler/semantic/semantic_state/v2/library/event_context/event_context.h"

namespace tz {

EventSite *EventContext::findSite(const std::string &key) {
  auto found = this->sites.find(key);

  return found == this->sites.end() ? nullptr : &found->second;
}

EventSite *EventContext::addSite(EventSite site) {
  std::string key = site.key;
  const Type *map = site.map;
  auto placed = this->sites.emplace(key, std::move(site)).first;
  this->mapSites[map] = key;

  return &placed->second;
}

EventSite *EventContext::findSiteOfMap(const Type *map) {
  auto found = this->mapSites.find(map);

  return found == this->mapSites.end() ? nullptr : this->findSite(found->second);
}

std::vector<EventSite *> EventContext::getSites() {
  auto all = std::vector<EventSite *>();
  for (auto &entry : this->sites) {
    all.push_back(&entry.second);
  }

  return all;
}

void EventContext::markSentEmitsFinished() {
  this->sentEmitsFinished = true;
}

bool EventContext::hasFinishedSentEmits() const {
  return this->sentEmitsFinished;
}

bool EventContext::hasCandidates(const Module *module) const {
  return this->candidates.count(module) > 0;
}

void EventContext::rememberCandidates(const Module *module, std::vector<EmitCandidate> found) {
  this->candidates[module] = std::move(found);
}

const std::vector<EmitCandidate> &EventContext::getCandidates(const Module *module) const {
  return this->candidates.at(module);
}

void EventContext::addErrorEmit(const Type *events, llvm::SMLoc loc) {
  for (const ErrorEmit &known : this->errorEmits) {
    if (known.loc == loc) {
      return;
    }
  }
  this->errorEmits.push_back({events, loc});
}

std::vector<ErrorEmit> EventContext::getErrorEmits() const {
  return this->errorEmits;
}

void EventContext::markErrorListened(const Type *events) {
  this->errorListenedMaps.insert(events);
}

bool EventContext::isErrorListened(const Type *events) const {
  return this->errorListenedMaps.contains(events);
}

void EventContext::addSelfListener(const ClassDecl *classDecl) {
  this->selfListeners.insert(classDecl);
}

bool EventContext::isSelfListener(const ClassDecl *classDecl) const {
  return this->selfListeners.contains(classDecl);
}

void EventContext::addDroppedNew(const ClassDecl *classDecl, llvm::SMLoc loc) {
  for (const DroppedNew &known : this->droppedNews) {
    if (known.loc == loc) {
      return;
    }
  }
  this->droppedNews.push_back({classDecl, loc});
}

std::vector<DroppedNew> EventContext::getDroppedNews() const {
  return this->droppedNews;
}

void EventContext::markErrorSubscriptionReported(llvm::SMLoc loc) {
  this->reportedErrorSubscriptions.insert(loc.getPointer());
}

bool EventContext::isErrorSubscriptionReported(llvm::SMLoc loc) const {
  return this->reportedErrorSubscriptions.contains(loc.getPointer());
}

}
