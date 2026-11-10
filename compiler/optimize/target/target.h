#pragma once

#include "compiler/program/diagnostic/diagnostic.h"
#include "compiler/result/result/result.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/TargetParser/Triple.h"
#include <memory>
#include <vector>

namespace tz {

class Target {
public:
  static Result<Target, std::vector<Diagnostic>> host(bool optimize);

  const llvm::Triple &triple() const { return triple_; }
  llvm::TargetMachine &machine() const { return *machine_; }

private:
  static void initializeTargets();
  Target(llvm::Triple triple, std::unique_ptr<llvm::TargetMachine> machine);

  llvm::Triple triple_;
  std::unique_ptr<llvm::TargetMachine> machine_;
};

}
