#pragma once

#include "compiler/semantic/feature/feature.h"

namespace tz {

class StreamFeature : public Feature {
public:
  llvm::StringRef name() const override { return "stream"; }

  Expr *checkLibraryCall(SemanticState &state, CallExpr *call, NameExpr *name,
                         FuncSymbol *func) const override;
};

}
