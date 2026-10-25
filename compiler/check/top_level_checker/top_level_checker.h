#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// The top-level code of each file, and the files a read loads early.
class TopLevelChecker {
public:
  explicit TopLevelChecker(Sema &sema);

  // Checks the top-level code of `module` from where it stopped: to the end,
  // or to the declaration of the global `until`.
  void checkTopLevel(Module *module, llvm::StringRef until = {});
  // A value by its name; if it is a global of a file that loads later, in
  // an import cycle, that file is checked up to its declaration first, out
  // of turn, when the code here does not run as the file loads: an arrow
  // that uses it, or a function whose return type is needed.
  Sema::ScopeEntry lookupValue(llvm::StringRef name);
  // The file that declares `name` if it is an import of module_ or a global
  // of module_ while another file loads, and its name there.
  Module *laterSource(llvm::StringRef name, llvm::StringRef &declared);
  bool loadEarly(Module *source, llvm::StringRef name);
  // Reports a name of a file that is checked later, in an import cycle: one
  // that is imported, or a global of a file whose function is checked early.
  // False if `name` is no such name.
  bool reportNotLoaded(const NameExpr *name);
  // `reader` reads `what`, declared as `declared` in `source`, which loads
  // after it.
  static std::string readBeforeLoad(const Module *reader, const std::string &what,
                                    const Module *source, llvm::StringRef declared);
  bool reportImportAssign(const NameExpr *name, const VarSymbol *var);
  // "main.tz -> a.tz -> b.tz -> (a.tz already started)": how `reader`
  // came to load before `source`.
  static std::string loadChain(const Module *reader, const Module *source);

private:
  Sema &sema_;
};

} // namespace tz
