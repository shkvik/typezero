#include "compiler/library/events/events_feature/events_feature.h"
#include "compiler/check/checker/checker.h"
#include "compiler/library/events/event_target_checker/event_target_checker.h"
#include "compiler/lower/lowering/lowering.h"

namespace tz {

Expr *EventsFeature::checkLibraryCall(Checker &checker, CallExpr *call, NameExpr *name,
                                      FuncSymbol *func) const {
  return checker.eventTargetChecker().checkLibraryCall(call, name, func);
}

}
