#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// The calls of typed arrays, DataView, Buffer, TextEncoder and TextDecoder.
class BytesCallLowerer {
public:
  explicit BytesCallLowerer(CodeGen &codegen);

  // Binary data (codegen_bytes.cpp). A typed array, a Buffer or a DataView
  // is a pointer to a TzBytes, an ArrayBuffer to a TzArrayBuffer
  // (runtime/core/tzrt/tzrt.h); both have their data pointer and their length
  // at the same offsets. The code reads and writes elements itself.
  llvm::Value *emitBytesCall(CallExpr *call);

private:
  CodeGen &codegen_;
  std::map<std::u16string, llvm::Constant *> embeddedBytes_;
};

} // namespace tz
