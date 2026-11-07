#pragma once

#include "compiler/link/licenses/licenses.h"
#include "compiler/program/errors/errors.h"

namespace tz {

class Target;
struct BuildOptions;
struct ObjectFile;

class LinkPass {
public:
  LinkPass(const Target &target, const BuildOptions &options);

  Errors run(const ObjectFile &object, const CryptoUse &crypto, bool parallel) const;

private:
  static bool inUse(llvm::StringRef path);
  const Target &target_;
  const BuildOptions &options_;
};

}
