#pragma once

#include "compiler/emit/object_file/object_file.h"
#include "compiler/program/errors/errors.h"
#include "compiler/result/result/result.h"
#include "llvm/IR/Module.h"

namespace tz {

class Target;

class EmitPass {
public:
  explicit EmitPass(const Target &target);

  Result<ObjectFile, Errors> run(llvm::Module &module) const;

private:
  const Target &target_;
};

}
