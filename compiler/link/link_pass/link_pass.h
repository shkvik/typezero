#pragma once

#include "compiler/link/licenses/licenses.h"
#include "compiler/program/errors/errors.h"

namespace tz {

class Target;
struct BuildOptions;
struct ObjectFile;

// The executable: the object file of the program, the runtime and, for node:crypto, its glue and
// OpenSSL, linked by the LLD built into tz. On success, what is left are the linker's warnings.
class LinkPass {
public:
  LinkPass(const Target &target, const BuildOptions &options);

  Errors run(const ObjectFile &object, const CryptoUse &crypto, bool parallel) const;

private:
  const Target &target_;
  const BuildOptions &options_;
};

} // namespace tz
