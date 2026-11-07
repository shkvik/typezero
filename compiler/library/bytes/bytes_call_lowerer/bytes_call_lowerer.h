#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class BytesCallLowerer {
public:
  explicit BytesCallLowerer(Lowering &owner);

  llvm::Value *emitBytesCall(CallExpr *call);

private:
  llvm::Value *emitEmbedBytes(CallExpr *call);

  Lowering &lowering;
  std::map<std::u16string, llvm::Constant *> embeddedBytes;
};

}
