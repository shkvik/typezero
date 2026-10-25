#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// The fields and methods of unions, and in.
class UnionMemberLowerer {
public:
  explicit UnionMemberLowerer(CodeGen &codegen);

  llvm::Value *emitUnionFieldRead(MemberExpr *member);
  // The address of field `name` of an object of the union `type`, a field of
  // one type in every member.
  llvm::Value *unionFieldAddress(llvm::Value *object, const Type *type, llvm::StringRef name);
  // `"radius" in s` of a union.
  llvm::Value *emitObjectIn(BinaryExpr *bin);
  // `s.area()` with the method of each member (see CallExpr::unionMethods).
  llvm::Value *emitUnionMethodCall(CallExpr *call);

private:
  CodeGen &codegen_;
};

} // namespace tz
