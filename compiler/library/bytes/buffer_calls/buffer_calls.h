#pragma once

#include "compiler/library/bytes/bytes_call_site/bytes_call_site.h"

namespace tz {

class BufferCalls : public BytesCallSite {
public:
  using BytesCallSite::BytesCallSite;

  llvm::Value *emit();
};

}
