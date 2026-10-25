#include "compiler/lower/errors/unwind_lowerer/unwind_lowerer.h"
#include "compiler/lower/function_frame/function_frame.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "compiler/lower/async/async_lowerer/async_lowerer.h"
#include "compiler/library/process/process_lowerer/process_lowerer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include <functional>
#include <map>
#include <optional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::BasicBlock;
using llvm::Value;

namespace tz {

UnwindLowerer::UnwindLowerer(CodeGen &codegen) : codegen_(codegen) {}

// As V8 names them: `f`, `Point.length`, `new Point`; nothing for top-level
// code and a function without a name.
std::string UnwindLowerer::frameFunctionName() const {
  const FunctionDecl *fn = codegen_.currentFn_;
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
// TzFrame in runtime/core/tzrt/tzrt.h.
llvm::Constant *UnwindLowerer::frameDescriptor(llvm::SMLoc loc, const std::string *name) {
  std::string function = name ? *name : frameFunctionName();
  auto key = std::make_pair(function, loc.getPointer());
  if (auto found = frames_.find(key); found != frames_.end()) {
    return found->second;
  }
  std::string file;
  unsigned line = 0, column = 0;
  if (codegen_.sourceMgr_ && loc.isValid()) {
    if (unsigned buffer = codegen_.sourceMgr_->FindBufferContainingLoc(loc)) {
      std::tie(line, column) = codegen_.sourceMgr_->getLineAndColumn(loc, buffer);
      llvm::StringRef id = codegen_.sourceMgr_->getMemoryBuffer(buffer)->getBufferIdentifier();
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
  llvm::Type *ptr = codegen_.builder_.getPtrTy(), *i32 = codegen_.builder_.getInt32Ty();
  auto *type = llvm::StructType::get(codegen_.ctx_, {ptr, i32, i32, ptr, i32, i32});
  llvm::Constant *init = llvm::ConstantStruct::get(
      type, {codegen_.stringLowerer().utf8Constant(function), codegen_.builder_.getInt32(uint32_t(function.size())),
             codegen_.builder_.getInt32(line), codegen_.stringLowerer().utf8Constant(file), codegen_.builder_.getInt32(uint32_t(file.size())),
             codegen_.builder_.getInt32(column)});
  auto *global = new llvm::GlobalVariable(*codegen_.module_, type, /*isConstant=*/true,
                                          llvm::GlobalValue::PrivateLinkage, init, "tz.frame");
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  frames_[key] = global;

  return global;
}

llvm::BasicBlock *UnwindLowerer::unwindTarget() {
  assert(!codegen_.scopes_.empty() && "code that may throw runs in a scope");

  return unwindBlock(codegen_.scopes_.size() - 1);
}

llvm::BasicBlock *UnwindLowerer::unwindBlock(size_t i) {
  CodeGen::Scope &scope = codegen_.scopes_[i];
  if (!scope.unwind) {
    scope.unwind = codegen_.functionFrame().newBlock("unwind");
  }

  return scope.unwind;
}

// After scope `i`: the `catch` or `finally` of a `try` whose block (or
// `catch`) it is, or the scope around it, or the way out of the function.
llvm::BasicBlock *UnwindLowerer::outerUnwind(size_t i) {
  for (CodeGen::TryContext &t : llvm::reverse(codegen_.tries_)) {
    if (t.scopeDepth != i) {
      continue;
    }
    if (t.phase == CodeGen::TryContext::Phase::Block) {
      return t.handler ? t.handler : t.throwEntry;
    }
    if (t.phase == CodeGen::TryContext::Phase::Handler && t.throwEntry) {
      return t.throwEntry;
    }
  }
  if (i > 0) {
    return unwindBlock(i - 1);
  }
  if (!codegen_.unwindExit_) {
    codegen_.unwindExit_ = codegen_.functionFrame().newBlock("unwind.exit");
  }

  return codegen_.unwindExit_;
}

void UnwindLowerer::finishScopeUnwind(size_t i) {
  CodeGen::Scope &scope = codegen_.scopes_[i];
  llvm::BasicBlock *unwind = std::exchange(scope.unwind, nullptr);
  if (!unwind) {
    return;
  }
  llvm::IRBuilderBase::InsertPoint point = codegen_.builder_.saveIP();
  codegen_.function_->insert(codegen_.function_->end(), unwind);
  codegen_.builder_.SetInsertPoint(unwind);
  codegen_.functionFrame().emitScopeCleanup(scope);
  codegen_.builder_.CreateBr(outerUnwind(i));
  codegen_.builder_.restoreIP(point);
}

// Out of the function: a generator is done, top-level code reports the
// error, and anything else returns zero, which its caller drops.
void UnwindLowerer::finishFunctionUnwind() {
  llvm::BasicBlock *exit = std::exchange(codegen_.unwindExit_, nullptr);
  if (!exit) {
    return;
  }
  llvm::IRBuilderBase::InsertPoint point = codegen_.builder_.saveIP();
  codegen_.function_->insert(codegen_.function_->end(), exit);
  codegen_.builder_.SetInsertPoint(exit);
  llvm::Type *ret = codegen_.function_->getReturnType();
  if (codegen_.async_) {
    codegen_.asyncLowerer().emitAsyncUnwind();
  } else if (codegen_.coro_) {
    codegen_.builder_.CreateBr(codegen_.coro_->finalBlock);
  } else if (codegen_.function_->hasExternalLinkage()) { // main
    // Node reports the error of an ES module a few turns of the microtask
    // queue later (runtime/core/async/async.c).
    llvm::CallInst *call = codegen_.builder_.CreateCall(
        codegen_.processLowerer().processFunction(codegen_.usesAsync_ && codegen_.esModule_ ? "tz_async_main_failed" : "tz_uncaught",
                        codegen_.builder_.getVoidTy(), {}));
    call->setDoesNotReturn();
    codegen_.builder_.CreateUnreachable();
  } else if (ret->isVoidTy()) {
    codegen_.builder_.CreateRetVoid();
  } else {
    codegen_.builder_.CreateRet(llvm::Constant::getNullValue(ret));
  }
  codegen_.builder_.restoreIP(point);
}

} // namespace tz
