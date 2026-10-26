#pragma once

#include "compiler/library/bytes/bytes_call_site/bytes_call_site.h"

namespace tz {

class NumberAccessCalls : public BytesCallSite {
public:
  using BytesCallSite::BytesCallSite;

  llvm::Value *emit();
};

}
