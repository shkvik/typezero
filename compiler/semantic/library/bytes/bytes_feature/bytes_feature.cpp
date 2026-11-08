#include "compiler/semantic/library/bytes/bytes_feature/bytes_feature.h"
#include "compiler/semantic/library/bytes/buffer_exports/buffer_exports.h"
#include "compiler/semantic/library/bytes/buffer_static_checker/buffer_static_checker.h"
#include "compiler/semantic/library/bytes/bytes_checker/bytes_checker.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

Expr *BytesFeature::checkStaticCall(SemanticState &state, CallExpr *call, MemberExpr *member,
                                    const Type *) const {
  return BufferStaticChecker(state).checkBytesStatic(call, member);
}

Expr *BytesFeature::checkMethodCall(SemanticState &state, CallExpr *call, MemberExpr *member,
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

}
