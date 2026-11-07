#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class LibraryCalls {
public:
  explicit LibraryCalls(Checker &owner);

  void prepareLibraries(SourceFile &program);
  bool inLibrary(llvm::SMLoc loc) const;
  Expr *checkLibCall(CallExpr *call, NameExpr *name);

private:
  Checker &checker;
  std::vector<std::pair<const char *, const char *>> libraryRanges;
};

}
