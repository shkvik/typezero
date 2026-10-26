#include "compiler/library/stream/stream_feature/stream_feature.h"
#include "compiler/check/checker/checker.h"
#include "compiler/library/stream/stream_pipeline/stream_pipeline.h"
#include "compiler/lower/lowering/lowering.h"

namespace tz {

Expr *StreamFeature::checkLibraryCall(Checker &checker, CallExpr *call, NameExpr *name,
                                      FuncSymbol *func) const {
  return checker.streamPipeline().checkLibraryCall(call, name, func);
}

}
