#pragma once

#include "compiler/library/bytes/bytes_call_site/bytes_call_site.h"

namespace tz {

// The properties and methods of typed arrays and ArrayBuffer.
class TypedArrayCalls : public BytesCallSite {
public:
  using BytesCallSite::BytesCallSite;

  llvm::Value *emit();
};

} // namespace tz
