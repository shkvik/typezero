#include "compiler/semantic/library/stream/stream_feature/stream_feature.h"
#include "compiler/semantic/library/stream/stream_pipeline/stream_pipeline.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

Expr *StreamFeature::checkLibraryCall(SemanticState &state, CallExpr *call, NameExpr *name,
                                      FuncSymbol *func) const {
  return StreamPipeline(state).checkLibraryCall(call, name, func);
}

}
