#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/tz_modules/library_calls/library_calls_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class LibraryCalls {
public:
  explicit LibraryCalls(CheckState &owner);

  void prepareLibraries(SourceFile &program);
  bool inLibrary(llvm::SMLoc loc) const;
  Expr *checkLibCall(CallExpr *call, NameExpr *name);

private:
  using ResultIntrinsicShape = Result<const CallExpr *, LibraryCallsIntrinsicShapeError>;

  static ResultIntrinsicShape intrinsicShape(const CallExpr *call, const NameExpr *name,
                                             llvm::StringRef intrinsicName, bool known,
                                             unsigned args, unsigned typeArgs);

  CheckState &state;
};

}
