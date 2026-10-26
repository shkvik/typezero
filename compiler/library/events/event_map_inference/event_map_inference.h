#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class EventMapInference {
public:
  explicit EventMapInference(Checker &checker);

  const Type *inferredEventMap(const std::string &site);
  std::string placeOf(llvm::SMLoc loc) const;
  EventInference::Portable portable(const Type *type, llvm::SmallPtrSetImpl<const Type *> &seen);
  const Type *fromPortable(const EventInference::Portable &p, llvm::SMLoc loc);

private:
  Checker &checker_;
  std::map<std::string, const Type *> siteMaps_;
};

}
