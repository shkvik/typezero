#include "compiler/library/bytes/bytes_feature/bytes_feature.h"
#include "compiler/check/checker/checker.h"
#include "compiler/library/bytes/buffer_static_checker/buffer_static_checker.h"
#include "compiler/library/bytes/bytes_call_lowerer/bytes_call_lowerer.h"
#include "compiler/library/bytes/bytes_checker/bytes_checker.h"
#include "compiler/lower/lowering/lowering.h"

namespace tz {

Expr *BytesFeature::checkStaticCall(Checker &checker, CallExpr *call, MemberExpr *member,
                                    const Type * /*expected*/) const {
  return checker.bufferStaticChecker().checkBytesStatic(call, member);
}

Expr *BytesFeature::checkMethodCall(Checker &checker, CallExpr *call, MemberExpr *member,
                                    const Type *expected) const {
  const Type *receiver = member->object->type;
  if (!receiver->isBytes()) {
    return nullptr;
  }

  return checker.bytesChecker().checkBytesMethod(call, member, expected);
}

bool BytesFeature::lowers(Builtin builtin) const { return isBytesBuiltin(builtin); }

llvm::Value *BytesFeature::lowerCall(Lowering &lowering, CallExpr *call) const {
  return lowering.bytesCallLowerer().emitBytesCall(call);
}

} // namespace tz
