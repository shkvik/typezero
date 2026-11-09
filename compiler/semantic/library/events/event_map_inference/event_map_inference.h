#pragma once

#include "compiler/semantic/flow/children_first/children_first.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "compiler/union/union/union.h"

namespace tz {

class SemanticParts;
class GenericClassInstantiator;

struct PortableHead {
  PortableForm form = PortableBad{};
  std::vector<const Type *> parts;
  bool keepsSeen = false;
};

enum class PortableStepKind { ENTER, LEAVE };

struct EnterPortable {
  static constexpr PortableStepKind kind = PortableStepKind::ENTER;
  const Type *type;
};

struct LeavePortable {
  static constexpr PortableStepKind kind = PortableStepKind::LEAVE;
  const Type *type;
  PortableHead head;
};

using PortableStep = Union<EnterPortable, LeavePortable>;

class EventMapInference {
public:
  EventMapInference(SemanticState &owner, SemanticParts &all);

  const Type *inferredEventMap(const std::string &site);
  std::string placeOf(llvm::SMLoc loc) const;
  EventInference::Portable portable(const Type *type);
  const Type *fromPortable(const EventInference::Portable &p, llvm::SMLoc loc);

private:
  static std::string fileOfPlace(const std::string &place);
  static bool sameInEveryRound(const Type *type);
  static bool isSameEveryRoundPart(const Type *type);
  static void leavePortable(const LeavePortable &leave, llvm::SmallPtrSetImpl<const Type *> &seen,
                            std::vector<EventInference::Portable> &answers);
  static PortableHead tupleHead(const Type *type);
  static PortableHead objectHead(const Type *type);
  static NodeChildren<const EventInference::Portable *>
  portableParts(const EventInference::Portable *p);

  void enterPortable(const Type *type, llvm::SmallPtrSetImpl<const Type *> &seen,
                     std::vector<PortableStep> &steps,
                     std::vector<EventInference::Portable> &answers);
  PortableHead portableHead(const Type *type) const;
  PortableHead classHead(const Type *type) const;
  const Type *nodeFromPortable(const EventInference::Portable &p,
                               std::vector<const Type *> &answers, llvm::SMLoc loc);

  ClassDecl *portableClass(const std::string &file, const std::string &name);

  SemanticState &state;
  GenericClassInstantiator *genericClassInstantiator = nullptr;
};

}
