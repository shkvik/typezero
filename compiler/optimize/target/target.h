#pragma once

#include "compiler/program/errors/errors.h"
#include "compiler/result/result/result.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/TargetParser/Triple.h"
#include <memory>

namespace tz {

class Target {
public:
  static Result<Target, Errors> host(bool optimize);

  const llvm::Triple &triple() const { return triple_; }
  llvm::TargetMachine &machine() const { return *machine_; }

private:
  Target(llvm::Triple triple, std::unique_ptr<llvm::TargetMachine> machine);

  llvm::Triple triple_;
  std::unique_ptr<llvm::TargetMachine> machine_;
};

}
