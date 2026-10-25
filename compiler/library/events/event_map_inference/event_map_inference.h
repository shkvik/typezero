#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// The event maps inferred from emit(), carried from one round of the check to the next.
class EventMapInference {
public:
  explicit EventMapInference(Checker &checker);

  // Event maps found from emit() calls (see EventInference). The map of the
  // emitter at `site` ("new main.tz:3:13", "class main.tz:Server"): in a
  // round that looks for them an opaque type of its own, which the calls on
  // its emitters record into; in the last round the object type of the
  // events found.
  const Type *inferredEventMap(const std::string &site);
  // The place of `loc` in the program, "main.tz:3:13".
  std::string placeOf(llvm::SMLoc loc) const;
  EventInference::Portable portable(const Type *type, llvm::SmallPtrSetImpl<const Type *> &seen);
  const Type *fromPortable(const EventInference::Portable &p, llvm::SMLoc loc);

private:
  Checker &checker_;
  std::map<std::string, const Type *> siteMaps_;
};

} // namespace tz
