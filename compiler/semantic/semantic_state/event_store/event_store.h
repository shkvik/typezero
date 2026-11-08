#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "compiler/semantic/semantic_state/event_store/portable_type.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Support/SMLoc.h"
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace tz {

struct EventInference {
  struct Portable {
    PortableForm form = PortableSame{};
    std::vector<Portable> parts;
  };
  struct Event {
    std::string name;
    Portable args;
    std::string where;
  };
  std::map<std::string, std::vector<Event>> maps;
  bool open = false;
  bool changed = false;
  bool final = false;
};

enum class ListenerKind { Plain, Async, This };

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
  explicit EventStore(EventInference &rounds);

  void markInferenceOpen();
  bool isInferenceFinal() const;
  std::vector<EventInference::Event> getInferredEvents(const std::string &site) const;
  const EventInference::Event *findInferredEvent(const std::string &site,
                                                 llvm::StringRef name) const;
  void addInferredEvent(const std::string &site, EventInference::Event event);
  void changeInferredEvent(const std::string &site, llvm::StringRef name,
                           EventInference::Portable args);
  void rememberSiteMap(const std::string &site, const Type *map);
  const Type *findSiteMap(const std::string &site) const;
  void rememberOpenMap(const Type *map, const std::string &site);
  bool isOpenMap(const Type *map) const;
  std::optional<std::string> findOpenMapSite(const Type *map) const;
  void rememberInferredMap(const Type *map, const std::string &site);
  bool isInferredMap(const Type *map) const;
  std::optional<std::string> findInferredMapSite(const Type *map) const;
  void addErrorEmit(const Type *events, llvm::SMLoc loc);
  std::vector<ErrorEmit> getErrorEmits() const;
  void markErrorListened(const Type *events);
  bool isErrorListened(const Type *events) const;
  void addSelfListener(const ClassDecl *cls);
  bool isSelfListener(const ClassDecl *cls) const;
  void addDroppedNew(const ClassDecl *cls, llvm::SMLoc loc);
  std::vector<DroppedNew> getDroppedNews() const;
  void markErrorSubscriptionReported(llvm::SMLoc loc);
  bool isErrorSubscriptionReported(llvm::SMLoc loc) const;

private:
  EventInference &inference;
  llvm::DenseMap<const Type *, std::string> openMaps;
  llvm::DenseMap<const Type *, std::string> inferredMaps;
  std::map<std::string, const Type *> siteMaps;
  std::vector<ErrorEmit> errorEmits;
  llvm::SmallPtrSet<const Type *, 4> errorListenedMaps;
  llvm::SmallPtrSet<const ClassDecl *, 4> selfListeners;
  std::vector<DroppedNew> droppedNews;
  llvm::DenseSet<const void *> reportedErrorSubscriptions;
};

}
