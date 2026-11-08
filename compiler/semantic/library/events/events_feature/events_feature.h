#pragma once

#include "compiler/semantic/feature/feature.h"

namespace tz {

class EventsFeature : public Feature {
public:
  llvm::StringRef name() const override { return "events"; }

  Expr *checkLibraryCall(SemanticState &state, CallExpr *call, NameExpr *name,
                         FuncSymbol *func) const override;
};

}
