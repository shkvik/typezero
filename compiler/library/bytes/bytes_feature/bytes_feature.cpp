#include "compiler/library/bytes/bytes_feature/bytes_feature.h"
#include "compiler/check/sema/sema.h"
#include "compiler/lower/codegen/codegen.h"
#include "compiler/library/bytes/buffer_static_checker/buffer_static_checker.h"
#include "compiler/library/bytes/bytes_checker/bytes_checker.h"
#include "compiler/library/bytes/bytes_call_lowerer/bytes_call_lowerer.h"

namespace tz {

Expr *BytesFeature::checkStaticCall(Sema &sema, CallExpr *call, MemberExpr *member, const Type *expected) const {
  return sema.bufferStaticChecker().checkBytesStatic(call, member);
}

Expr *BytesFeature::checkMethodCall(Sema &sema, CallExpr *call, MemberExpr *member, const Type *expected) const {
  const Type *receiver = member->object->type;
  if (!receiver->isBytes()) {
    return nullptr;
  }

  return sema.bytesChecker().checkBytesMethod(call, member, expected);
}

bool BytesFeature::lowers(Builtin builtin) const {
  return isBytesBuiltin(builtin);
}

llvm::Value *BytesFeature::lowerCall(CodeGen &codegen, CallExpr *call) const {
  return codegen.bytesCallLowerer().emitBytesCall(call);
}

} // namespace tz
