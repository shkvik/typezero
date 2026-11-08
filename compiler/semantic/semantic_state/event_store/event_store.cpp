#include "compiler/semantic/semantic_state/event_store/event_store.h"

namespace tz {

EventStore::EventStore(EventInference &rounds)
    : inference(rounds) {}

void EventStore::markInferenceOpen() {
  this->inference.open = true;
}

bool EventStore::isInferenceFinal() const {
  return this->inference.final;
}

std::vector<EventInference::Event> EventStore::getInferredEvents(const std::string &site) const {
  auto found = this->inference.maps.find(site);
  if (found == this->inference.maps.end()) {
    return {};
  }

  return found->second;
}

const EventInference::Event *EventStore::findInferredEvent(const std::string &site,
                                                           llvm::StringRef name) const {
  auto found = this->inference.maps.find(site);
  if (found == this->inference.maps.end()) {
    return nullptr;
  }
  for (const EventInference::Event &event : found->second) {
    if (event.name == name) {
      return &event;
    }
  }

  return nullptr;
}

void EventStore::addInferredEvent(const std::string &site, EventInference::Event event) {
  this->inference.maps[site].push_back(std::move(event));
  this->inference.changed = true;
}

void EventStore::changeInferredEvent(const std::string &site, llvm::StringRef name,
                                     EventInference::Portable args) {
  for (EventInference::Event &event : this->inference.maps[site]) {
    if (event.name == name) {
      event.args = std::move(args);
      this->inference.changed = true;
      return;
    }
  }
}

void EventStore::rememberSiteMap(const std::string &site, const Type *map) {
  this->siteMaps[site] = map;
}

const Type *EventStore::findSiteMap(const std::string &site) const {
  auto found = this->siteMaps.find(site);
  if (found == this->siteMaps.end()) {
    return nullptr;
  }

  return found->second;
}

void EventStore::rememberOpenMap(const Type *map, const std::string &site) {
  this->openMaps[map] = site;
}

bool EventStore::isOpenMap(const Type *map) const {
  return this->openMaps.count(map) > 0;
}

std::optional<std::string> EventStore::findOpenMapSite(const Type *map) const {
  auto found = this->openMaps.find(map);
  if (found == this->openMaps.end()) {
    return std::nullopt;
  }

  return found->second;
}

void EventStore::rememberInferredMap(const Type *map, const std::string &site) {
  this->inferredMaps[map] = site;
}

bool EventStore::isInferredMap(const Type *map) const {
  return this->inferredMaps.count(map) > 0;
}

std::optional<std::string> EventStore::findInferredMapSite(const Type *map) const {
  auto found = this->inferredMaps.find(map);
  if (found == this->inferredMaps.end()) {
    return std::nullopt;
  }

  return found->second;
}

void EventStore::addErrorEmit(const Type *events, llvm::SMLoc loc) {
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
