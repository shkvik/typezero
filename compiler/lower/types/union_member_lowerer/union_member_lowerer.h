#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

// The fields and methods of unions, and in.
class UnionMemberLowerer {
public:
  explicit UnionMemberLowerer(Lowering &lowering);

  llvm::Value *emitUnionFieldRead(MemberExpr *member);
  // The address of field `name` of an object of the union `type`, a field of
  // one type in every member.
  llvm::Value *unionFieldAddress(llvm::Value *object, const Type *type, llvm::StringRef name);
  // `"radius" in s` of a union.
  llvm::Value *emitObjectIn(BinaryExpr *bin);
  // `s.area()` with the method of each member (see CallExpr::unionMethods).
  llvm::Value *emitUnionMethodCall(CallExpr *call);

private:
  Lowering &lowering_;
};

} // namespace tz
