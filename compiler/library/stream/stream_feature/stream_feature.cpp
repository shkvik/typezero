#include "compiler/library/stream/stream_feature/stream_feature.h"
#include "compiler/lower/lowering/lowering.h"
#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/stream/stream_pipeline/stream_pipeline.h"

namespace tz {

Expr *StreamFeature::checkLibraryCall(CheckState &state, CallExpr *call, NameExpr *name,
                                      FuncSymbol *func) const {
  return StreamPipeline(state).checkLibraryCall(call, name, func);
}

}
