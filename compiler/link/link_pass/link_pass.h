#pragma once

#include "compiler/link/licenses/licenses.h"
#include "compiler/native/native_pass/native_link.h"
#include "compiler/program/diagnostic/diagnostic.h"
#include "compiler/result/result/result.h"
#include <vector>

namespace tz {

class Target;
struct BuildOptions;
struct ObjectFile;

class LinkPass {
public:
  using ResultLinked = Result<std::vector<Diagnostic>, std::vector<Diagnostic>>;

  LinkPass(const Target &target, const BuildOptions &options);

  ResultLinked run(const ObjectFile &object, const CryptoUse &crypto, bool parallel,
                   const NativeLink &native, llvm::ArrayRef<ForeignFunction> foreign) const;

private:
  static bool inUse(llvm::StringRef path);
  std::vector<Diagnostic> foreignErrors(llvm::StringRef messages,
                                        llvm::ArrayRef<ForeignFunction> foreign) const;
  const ForeignFunction *findForeign(llvm::StringRef name,
                                     llvm::ArrayRef<ForeignFunction> foreign) const;
  const Target &target;
  const BuildOptions &options;
};

}
