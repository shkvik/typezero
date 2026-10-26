#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class UnionMemberLowerer {
public:
  explicit UnionMemberLowerer(Lowering &lowering);

  llvm::Value *emitUnionFieldRead(MemberExpr *member);
  llvm::Value *unionFieldAddress(llvm::Value *object, const Type *type, llvm::StringRef name);
  llvm::Value *emitObjectIn(BinaryExpr *bin);
  llvm::Value *emitUnionMethodCall(CallExpr *call);

private:
  Lowering &lowering_;
};

}
