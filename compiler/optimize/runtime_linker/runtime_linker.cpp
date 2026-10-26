#include "compiler/optimize/runtime_linker/runtime_linker.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/MemoryBuffer.h"

namespace tz {

namespace {

constexpr unsigned kRuntimeCopyLimit = 100;

unsigned copySize(llvm::Function &fn, llvm::SmallPtrSetImpl<llvm::Function *> &seen) {
  unsigned size = fn.getInstructionCount();
  for (llvm::Instruction &inst : llvm::instructions(fn)) {
    for (llvm::Value *operand : inst.operands()) {
      auto *callee = llvm::dyn_cast<llvm::Function>(operand);
      if (callee && callee->hasLocalLinkage() && !callee->isDeclaration() &&
          seen.insert(callee).second) {
        size += copySize(*callee, seen);
      }
    }
  }

  return size;
}

}

Errors RuntimeLinker::run(llvm::Module &module, const std::string &path) const {
  Errors errors;
  auto buffer = llvm::MemoryBuffer::getFile(path);
  if (!buffer) {
    errors.tool("the runtime is missing: " + path);
    return errors;
  }
  llvm::Expected<std::unique_ptr<llvm::Module>> runtime =
      llvm::getOwningLazyBitcodeModule(std::move(*buffer), module.getContext());
  if (!runtime) {
    errors.tool("cannot read the runtime " + path + ": " + llvm::toString(runtime.takeError()));
    return errors;
  }
  (*runtime)->setTargetTriple(module.getTargetTriple());
  (*runtime)->setDataLayout(module.getDataLayout());

  for (llvm::Function &decl : module) {
    llvm::Function *def = decl.isDeclaration() ? (*runtime)->getFunction(decl.getName()) : nullptr;
    if (def && def->getFunctionType() != decl.getFunctionType()) {
      errors.tool("internal error: " + decl.getName() +
                  " is declared differently in Lowering and in the runtime");
      return errors;
    }
  }
  llvm::StringSet<> shared;
  for (llvm::GlobalValue &value : (*runtime)->global_values()) {
    if (value.isDeclaration()) {
      continue;
    }
    auto *var = llvm::dyn_cast<llvm::GlobalVariable>(&value);
    if (var && var->hasLocalLinkage() && !var->isConstant() && !var->getName().starts_with("__")) {
      errors.tool("internal error: the runtime variable " + var->getName() + " is static");
      return errors;
    }
    if (value.hasExternalLinkage()) {
      shared.insert(value.getName());
    }
  }

  for (llvm::Function &fn : llvm::make_early_inc_range(module)) {
    if (fn.isDeclaration() && fn.use_empty()) {
      fn.eraseFromParent();
    }
  }
  if (llvm::Linker::linkModules(module, std::move(*runtime), llvm::Linker::LinkOnlyNeeded)) {
    errors.tool("internal error: cannot link the runtime");
    return errors;
  }

  for (llvm::Function &fn : module) {
    fn.removeFnAttr("target-cpu");
    fn.removeFnAttr("target-features");
    fn.removeFnAttr("tune-cpu");
    if (fn.isDeclaration() || !shared.contains(fn.getName())) {
      continue;
    }
    fn.setComdat(nullptr);
    llvm::SmallPtrSet<llvm::Function *, 8> helpers;
    if (!fn.hasFnAttribute(llvm::Attribute::Hot) || copySize(fn, helpers) > kRuntimeCopyLimit) {
      fn.deleteBody();
    } else {
      fn.setLinkage(llvm::GlobalValue::AvailableExternallyLinkage);
    }
  }
  for (llvm::GlobalVariable &var : module.globals()) {
    if (var.isDeclaration() || !shared.contains(var.getName())) {
      continue;
    }
    var.setComdat(nullptr);
    if (var.isConstant()) {
      var.setLinkage(llvm::GlobalValue::AvailableExternallyLinkage);
    } else {
      var.setInitializer(nullptr);
      var.setLinkage(llvm::GlobalValue::ExternalLinkage);
    }
  }

  return errors;
}

}
