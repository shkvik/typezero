#pragma once

#include "compiler/library/feature/feature.h"

namespace tz {

// node:stream (docs/streams.md): pipeline() and the other functions of its modules.
class StreamFeature : public Feature {
public:
  llvm::StringRef name() const override { return "stream"; }

  Expr *checkLibraryCall(Sema &sema, CallExpr *call, NameExpr *name, FuncSymbol *func) const override;
};

} // namespace tz
