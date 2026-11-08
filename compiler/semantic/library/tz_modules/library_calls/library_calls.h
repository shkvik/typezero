#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/tz_modules/library_calls/library_calls_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class LibraryCalls {
public:
  explicit LibraryCalls(SemanticState &owner);

  void prepareLibraries(SourceFile &program);
  bool inLibrary(llvm::SMLoc loc) const;
  Expr *checkLibCall(CallExpr *call, NameExpr *name);

private:
  using ResultIntrinsicShape = Result<const CallExpr *, LibraryCallsIntrinsicShapeError>;

  static ResultIntrinsicShape intrinsicShape(const CallExpr *call, const NameExpr *name,
                                             llvm::StringRef intrinsicName, bool known,
                                             unsigned args, unsigned typeArgs);

  SemanticState &state;
};

}
