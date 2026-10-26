#pragma once

#include "compiler/library/feature/feature.h"

namespace tz {

class EventsFeature : public Feature {
public:
  llvm::StringRef name() const override { return "events"; }

  Expr *checkLibraryCall(Checker &checker, CallExpr *call, NameExpr *name,
                         FuncSymbol *func) const override;
};

}
