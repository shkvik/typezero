#pragma once

#include "compiler/library/feature/feature.h"

namespace tz {

// node:events and EventTarget (docs/events.md): once() and the other functions of the
// module.
class EventsFeature : public Feature {
public:
  llvm::StringRef name() const override { return "events"; }

  Expr *checkLibraryCall(Sema &sema, CallExpr *call, NameExpr *name, FuncSymbol *func) const override;
};

} // namespace tz
