#include "compiler/semantic/semantic_state/v2/types/widening_context/widening_context.h"

namespace tz {

void WideningContext::rememberWidening(const Type *from, const Type *to, bool narrows) {
  this->narrowingWidenings[{from, to}] = narrows;
}

bool WideningContext::hasKnownWidening(const Type *from, const Type *to) const {
  return this->narrowingWidenings.count({from, to}) > 0;
}

bool WideningContext::isNarrowingWidening(const Type *from, const Type *to) const {
  auto found = this->narrowingWidenings.find({from, to});

  return found != this->narrowingWidenings.end() && found->second;
}

}
