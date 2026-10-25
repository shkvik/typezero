#pragma once

#include "compiler/library/bytes/bytes_call_site/bytes_call_site.h"

namespace tz {

// The calls that make binary data: new of a typed array, an ArrayBuffer or a DataView, from a
// length, an array, other binary data or a list of values, and views of a buffer.
class BytesMakeCalls : public BytesCallSite {
public:
  using BytesCallSite::BytesCallSite;

  llvm::Value *emit();
};

} // namespace tz
