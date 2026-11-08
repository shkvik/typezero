#pragma once

#include "compiler/lower/library/bytes/bytes_call_site/bytes_call_site.h"

namespace tz {

class TextCodecCalls : public BytesCallSite {
public:
  using BytesCallSite::BytesCallSite;

  llvm::Value *emit();

private:
  static llvm::Constant *cjkEncodingOf(llvm::Module &module, llvm::StringRef name);
};

}
