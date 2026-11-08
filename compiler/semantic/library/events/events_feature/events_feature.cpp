#include "compiler/semantic/library/events/events_feature/events_feature.h"
#include "compiler/semantic/library/events/event_target_checker/event_target_checker.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

Expr *EventsFeature::checkLibraryCall(SemanticState &state, CallExpr *call, NameExpr *name,
                                      FuncSymbol *func) const {
  return EventTargetChecker(state).checkLibraryCall(call, name, func);
}

}
