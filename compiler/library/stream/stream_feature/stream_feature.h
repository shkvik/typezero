#pragma once

#include "compiler/library/feature/feature.h"

namespace tz {

class StreamFeature : public Feature {
public:
  llvm::StringRef name() const override { return "stream"; }

  Expr *checkLibraryCall(Checker &checker, CallExpr *call, NameExpr *name,
                         FuncSymbol *func) const override;
};

}
