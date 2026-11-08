#include "compiler/semantic/library/tz_modules/tz_modules_feature/tz_modules_feature.h"
#include "compiler/semantic/library/tz_modules/library_calls/library_calls.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

void TzModulesFeature::prepare(SemanticState &state, SourceFile &file) const {
  LibraryCalls(state).prepareLibraries(file);
}

Expr *TzModulesFeature::checkFunctionCall(SemanticState &state, CallExpr *call, NameExpr *name,
                                          const Type *) const {
  return LibraryCalls(state).checkLibCall(call, name);
}

}
