#include "compiler/library/stream/stream_feature/stream_feature.h"
#include "compiler/check/sema/sema.h"
#include "compiler/lower/codegen/codegen.h"
#include "compiler/library/stream/stream_pipeline/stream_pipeline.h"

namespace tz {

Expr *StreamFeature::checkLibraryCall(Sema &sema, CallExpr *call, NameExpr *name, FuncSymbol *func) const {
  return sema.streamPipeline().checkLibraryCall(call, name, func);
}

} // namespace tz
