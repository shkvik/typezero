#pragma once

#include "compiler/link/licenses/licenses.h"
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

  ResultLinked run(const ObjectFile &object, const CryptoUse &crypto, bool parallel) const;

private:
  static bool inUse(llvm::StringRef path);
  const Target &target;
  const BuildOptions &options;
};

}
