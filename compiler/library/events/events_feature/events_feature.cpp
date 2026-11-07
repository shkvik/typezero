#include "compiler/library/events/events_feature/events_feature.h"
#include "compiler/lower/lowering/lowering.h"
#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/events/event_target_checker/event_target_checker.h"

namespace tz {

Expr *EventsFeature::checkLibraryCall(CheckState &state, CallExpr *call, NameExpr *name,
                                      FuncSymbol *func) const {
  return EventTargetChecker(state).checkLibraryCall(call, name, func);
}

}
