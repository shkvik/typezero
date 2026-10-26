#include "compiler/optimize/target/target.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"
#include <optional>
#include <string>

namespace tz {

namespace {

void initializeTargets() {
  LLVMInitializeX86TargetInfo();
  LLVMInitializeX86Target();
  LLVMInitializeX86TargetMC();
  LLVMInitializeX86AsmPrinter();
  LLVMInitializeAArch64TargetInfo();
  LLVMInitializeAArch64Target();
  LLVMInitializeAArch64TargetMC();
  LLVMInitializeAArch64AsmPrinter();
}

}

Target::Target(llvm::Triple triple, std::unique_ptr<llvm::TargetMachine> machine)
    : triple_(std::move(triple)), machine_(std::move(machine)) {}

Result<Target, Errors> Target::host(bool optimize) {
  static bool initialized = (initializeTargets(), true);
  (void)initialized;
  llvm::Triple triple(llvm::sys::getDefaultTargetTriple());
  std::string error;
  const llvm::Target *target = llvm::TargetRegistry::lookupTarget(triple, error);
  if (!target) {
    Errors errors;
    errors.tool(error);
    return err(errors);
  }
  llvm::TargetOptions options;
  std::unique_ptr<llvm::TargetMachine> machine(target->createTargetMachine(
      triple, "generic", "", options, std::nullopt, std::nullopt,
      optimize ? llvm::CodeGenOptLevel::Default : llvm::CodeGenOptLevel::None));

  return ok(Target(triple, std::move(machine)));
}

}
