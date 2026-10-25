#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

// The calls of typed arrays, DataView, Buffer, TextEncoder and TextDecoder.
class BytesCallLowerer {
public:
  explicit BytesCallLowerer(Lowering &lowering);

  // Binary data (codegen_bytes.cpp). A typed array, a Buffer or a DataView
  // is a pointer to a TzBytes, an ArrayBuffer to a TzArrayBuffer
  // (runtime/library/bytes/bytes/bytes.h); both have their data pointer and their length
  // at the same offsets. The code reads and writes elements itself.
  llvm::Value *emitBytesCall(CallExpr *call);

private:
  // embedBytes(path): one constant Uint8Array for each file.
  llvm::Value *emitEmbedBytes(CallExpr *call);

  Lowering &lowering_;
  std::map<std::u16string, llvm::Constant *> embeddedBytes_;
};

} // namespace tz
