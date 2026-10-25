#pragma once

#include "compiler/library/bytes/bytes_call_site/bytes_call_site.h"

namespace tz {

// Numbers read from and written to bytes: the methods of DataView, and readInt32LE and the
// like of Buffer.
class NumberAccessCalls : public BytesCallSite {
public:
  using BytesCallSite::BytesCallSite;

  llvm::Value *emit();
};

} // namespace tz
