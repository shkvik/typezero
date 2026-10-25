#include "compiler/optimize/runtime_linker/runtime_linker.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/MemoryBuffer.h"

namespace tz {

namespace {

// A hot runtime function with more instructions than this, counting the
// static helpers it brings along, is not copied into programs after all: the
// inliner would rarely take it, and optimizing it again for every program
// only costs compile time.
constexpr unsigned kRuntimeCopyLimit = 100;

// The instructions of `fn` and of the static functions it refers to, which
// would be copied with it.
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

} // namespace

Errors RuntimeLinker::run(llvm::Module &module, const std::string &path) const {
  Errors errors;
  auto buffer = llvm::MemoryBuffer::getFile(path);
  if (!buffer) {
    errors.tool("the runtime is missing: " + path);
    return errors;
  }
  // Lazily: only the functions that end up linked are read in full.
  llvm::Expected<std::unique_ptr<llvm::Module>> runtime =
      llvm::getOwningLazyBitcodeModule(std::move(*buffer), module.getContext());
  if (!runtime) {
    errors.tool("cannot read the runtime " + path + ": " + llvm::toString(runtime.takeError()));
    return errors;
  }
  (*runtime)->setTargetTriple(module.getTargetTriple());
  (*runtime)->setDataLayout(module.getDataLayout());

  // A call through a declaration that disagrees with the definition would
  // not be inlined, or would pass its arguments wrongly.
  for (llvm::Function &decl : module) {
    llvm::Function *def = decl.isDeclaration() ? (*runtime)->getFunction(decl.getName()) : nullptr;
    if (def && def->getFunctionType() != decl.getFunctionType()) {
      errors.tool("internal error: " + decl.getName() +
                  " is declared differently in CodeGen and in the runtime");
      return errors;
    }
  }
  // What the copies refer to by name is the one copy in tzrt.lib. State that
  // is static would be duplicated instead: every copied function would get a
  // variable of its own. (Names starting with __ belong to the C library's
  // inline functions.)
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

  // CodeGen declares every runtime function; only those the program calls
  // are to be linked.
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
    // clang named a CPU for the runtime; the inliner does not mix functions
    // built for different CPUs, so everything uses the target's default.
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
  // Static helpers that only the dropped bodies called are left over;
  // OptimizePass removes them before optimizing.

  return errors;
}

} // namespace tz
