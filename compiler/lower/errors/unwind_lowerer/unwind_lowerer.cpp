#include "compiler/lower/errors/unwind_lowerer/unwind_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "compiler/lower/async/async_lowerer/async_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include <functional>
#include <map>
#include <optional>

using llvm::BasicBlock;
using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::Value;

namespace tz {

UnwindLowerer::UnwindLowerer(Lowering &lowering) : lowering_(lowering) {}

// As V8 names them: `f`, `Point.length`, `new Point`; nothing for top-level
// code and a function without a name.
std::string UnwindLowerer::frameFunctionName() const {
  const FunctionDecl *fn = lowering_.currentFn_;
  if (!fn) {
    return "";
  }
  if (fn->cls && !fn->cls->literal) {
    if (fn->isConstructor || fn == fn->cls->init) {
      return "new " + fn->cls->name.str();
    }
    return (fn->cls->name + "." + fn->name).str();
  }

  return fn->name.str();
}

// { ptr function, i32 length, i32 line, ptr file, i32 length, i32 column }:
// TzFrame in runtime/core/error/error.h.
llvm::Constant *UnwindLowerer::frameDescriptor(llvm::SMLoc loc, const std::string *name) {
  std::string function = name ? *name : frameFunctionName();
  auto key = std::make_pair(function, loc.getPointer());
  if (auto found = frames_.find(key); found != frames_.end()) {
    return found->second;
  }
  std::string file;
  unsigned line = 0, column = 0;
  if (lowering_.sourceMgr_ && loc.isValid()) {
    if (unsigned buffer = lowering_.sourceMgr_->FindBufferContainingLoc(loc)) {
      std::tie(line, column) = lowering_.sourceMgr_->getLineAndColumn(loc, buffer);
      llvm::StringRef id = lowering_.sourceMgr_->getMemoryBuffer(buffer)->getBufferIdentifier();
      // The full path, as Node shows it.
      auto [it, inserted] = filePaths_.try_emplace(id);
      // A built-in module written in tz is named as node names its own,
      // `tz:http:12:5`.
      if (inserted && id.starts_with("tz:")) {
        it->second = id.str();
      } else if (inserted) {
        llvm::SmallString<256> path(id);
        llvm::sys::fs::make_absolute(path);
        llvm::sys::path::remove_dots(path, /*remove_dot_dot=*/true);
        llvm::sys::path::native(path);
        it->second = std::string(path);
      }
      file = it->second;
    }
  }
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i32 = lowering_.builder_.getInt32Ty();
  auto *type = llvm::StructType::get(lowering_.ctx_, {ptr, i32, i32, ptr, i32, i32});
  llvm::Constant *init = llvm::ConstantStruct::get(
      type,
      {lowering_.stringLowerer().utf8Constant(function),
       lowering_.builder_.getInt32(uint32_t(function.size())), lowering_.builder_.getInt32(line),
       lowering_.stringLowerer().utf8Constant(file),
       lowering_.builder_.getInt32(uint32_t(file.size())), lowering_.builder_.getInt32(column)});
  auto *global = new llvm::GlobalVariable(*lowering_.module_, type, /*isConstant=*/true,
                                          llvm::GlobalValue::PrivateLinkage, init, "tz.frame");
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  frames_[key] = global;

  return global;
}

llvm::BasicBlock *UnwindLowerer::unwindTarget() {
  assert(!lowering_.scopes_.empty() && "code that may throw runs in a scope");

  return unwindBlock(lowering_.scopes_.size() - 1);
}

llvm::BasicBlock *UnwindLowerer::unwindBlock(size_t i) {
  Lowering::Scope &scope = lowering_.scopes_[i];
  if (!scope.unwind) {
    scope.unwind = lowering_.functionFrame().newBlock("unwind");
  }

  return scope.unwind;
}

// After scope `i`: the `catch` or `finally` of a `try` whose block (or
// `catch`) it is, or the scope around it, or the way out of the function.
llvm::BasicBlock *UnwindLowerer::outerUnwind(size_t i) {
  for (Lowering::TryContext &t : llvm::reverse(lowering_.tries_)) {
    if (t.scopeDepth != i) {
      continue;
    }
    if (t.phase == Lowering::TryContext::Phase::Block) {
      return t.handler ? t.handler : t.throwEntry;
    }
    if (t.phase == Lowering::TryContext::Phase::Handler && t.throwEntry) {
      return t.throwEntry;
    }
  }
  if (i > 0) {
    return unwindBlock(i - 1);
  }
  if (!lowering_.unwindExit_) {
    lowering_.unwindExit_ = lowering_.functionFrame().newBlock("unwind.exit");
  }

  return lowering_.unwindExit_;
}

void UnwindLowerer::finishScopeUnwind(size_t i) {
  Lowering::Scope &scope = lowering_.scopes_[i];
  llvm::BasicBlock *unwind = std::exchange(scope.unwind, nullptr);
  if (!unwind) {
    return;
  }
  llvm::IRBuilderBase::InsertPoint point = lowering_.builder_.saveIP();
  lowering_.function_->insert(lowering_.function_->end(), unwind);
  lowering_.builder_.SetInsertPoint(unwind);
  lowering_.functionFrame().emitScopeCleanup(scope);
  lowering_.builder_.CreateBr(outerUnwind(i));
  lowering_.builder_.restoreIP(point);
}

// Out of the function: a generator is done, top-level code reports the
// error, and anything else returns zero, which its caller drops.
void UnwindLowerer::finishFunctionUnwind() {
  llvm::BasicBlock *exit = std::exchange(lowering_.unwindExit_, nullptr);
  if (!exit) {
    return;
  }
  llvm::IRBuilderBase::InsertPoint point = lowering_.builder_.saveIP();
  lowering_.function_->insert(lowering_.function_->end(), exit);
  lowering_.builder_.SetInsertPoint(exit);
  llvm::Type *ret = lowering_.function_->getReturnType();
  if (lowering_.async_) {
    lowering_.asyncLowerer().emitAsyncUnwind();
  } else if (lowering_.coro_) {
    lowering_.builder_.CreateBr(lowering_.coro_->finalBlock);
  } else if (lowering_.function_->hasExternalLinkage()) { // main
    // Node reports the error of an ES module a few turns of the microtask
    // queue later (runtime/core/async/async.c).
    llvm::CallInst *call = lowering_.builder_.CreateCall(lowering_.processLowerer().processFunction(
        lowering_.usesAsync_ && lowering_.esModule_ ? "tz_async_main_failed" : "tz_uncaught",
        lowering_.builder_.getVoidTy(), {}));
    call->setDoesNotReturn();
    lowering_.builder_.CreateUnreachable();
  } else if (ret->isVoidTy()) {
    lowering_.builder_.CreateRetVoid();
  } else {
    lowering_.builder_.CreateRet(llvm::Constant::getNullValue(ret));
  }
  lowering_.builder_.restoreIP(point);
}

} // namespace tz
