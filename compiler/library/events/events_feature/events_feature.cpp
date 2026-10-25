#include "compiler/library/events/events_feature/events_feature.h"
#include "compiler/check/sema/sema.h"
#include "compiler/lower/codegen/codegen.h"
#include "compiler/library/events/event_target_checker/event_target_checker.h"

namespace tz {

Expr *EventsFeature::checkLibraryCall(Sema &sema, CallExpr *call, NameExpr *name, FuncSymbol *func) const {
  return sema.eventTargetChecker().checkLibraryCall(call, name, func);
}

} // namespace tz
