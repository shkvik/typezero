#pragma once

#include "compiler/semantic/feature/feature.h"

namespace tz {

class TzModulesFeature : public Feature {
public:
  llvm::StringRef name() const override { return "tz_modules"; }

  void prepare(SemanticState &state, SourceFile &file) const override;
  Expr *checkFunctionCall(SemanticState &state, CallExpr *call, NameExpr *name,
                          const Type *expected) const override;
};

}
