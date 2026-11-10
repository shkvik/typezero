#pragma once

#include "compiler/emit/object_file/object_file.h"
#include "compiler/program/diagnostic/diagnostic.h"
#include "compiler/result/result/result.h"
#include "llvm/IR/Module.h"
#include <vector>

namespace tz {

class Target;

class EmitPass {
public:
  explicit EmitPass(const Target &target);

  Result<ObjectFile, std::vector<Diagnostic>> run(llvm::Module &module) const;

private:
  const Target &target;
};

}
