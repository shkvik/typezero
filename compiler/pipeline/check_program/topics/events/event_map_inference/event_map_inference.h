#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"

namespace tz {

class EventMapInference {
public:
  explicit EventMapInference(CheckState &owner);

  const Type *inferredEventMap(const std::string &site);
  std::string placeOf(llvm::SMLoc loc) const;
  EventInference::Portable portable(const Type *type, llvm::SmallPtrSetImpl<const Type *> &seen);
  const Type *fromPortable(const EventInference::Portable &p, llvm::SMLoc loc);

private:
  static std::string fileOfPlace(const std::string &place);
  static bool sameInEveryRound(const Type *type, llvm::SmallPtrSetImpl<const Type *> &seen);

  ClassDecl *portableClass(const std::string &file, const std::string &name);

  CheckState &state;
};

}
