#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"

namespace tz {

class LibraryCalls {
public:
  explicit LibraryCalls(CheckState &owner);

  void prepareLibraries(SourceFile &program);
  bool inLibrary(llvm::SMLoc loc) const;
  Expr *checkLibCall(CallExpr *call, NameExpr *name);

private:
  CheckState &state;
};

}
