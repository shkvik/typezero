#include "compiler/library/bytes/bytes_feature/bytes_feature.h"
#include "compiler/library/bytes/buffer_exports/buffer_exports.h"
#include "compiler/library/bytes/bytes_call_lowerer/bytes_call_lowerer.h"
#include "compiler/lower/lowering/lowering.h"
#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/bytes/buffer_static_checker/buffer_static_checker.h"
#include "compiler/pipeline/check_program/topics/bytes/bytes_checker/bytes_checker.h"

namespace tz {

Expr *BytesFeature::checkStaticCall(CheckState &state, CallExpr *call, MemberExpr *member,
                                    const Type *) const {
  return BufferStaticChecker(state).checkBytesStatic(call, member);
}

Expr *BytesFeature::checkMethodCall(CheckState &state, CallExpr *call, MemberExpr *member,
                                    const Type *expected) const {
  const Type *receiver = member->object->type;
  if (!receiver->isBytes()) {
    return nullptr;
  }

  return BytesChecker(state).checkBytesMethod(call, member, expected);
}

BytesFeature::ResultExportFound BytesFeature::exportFound(NodeModule module,
                                                          llvm::StringRef exportName,
                                                          llvm::StringRef path) const {
  if (module == NodeModule::Buffer) {
    return BufferExports::bufferExport(exportName, path);
  }

  return err(FeatureExportFoundError{
      .code = FeatureExportFoundErrorCode::NOT_MINE,
      .message = "",
  });
}

bool BytesFeature::lowers(Builtin builtin) const {
  return BuiltinGroups::isBytesBuiltin(builtin);
}

llvm::Value *BytesFeature::lowerCall(Lowering &lowering, CallExpr *call) const {
  return lowering.bytesCallLowerer().emitBytesCall(call);
}

}
