#include "compiler/lower/places/place_lowerer/place_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include <map>

namespace tz {

PlaceLowerer::PlaceLowerer(Lowering &lowering) : lowering_(lowering) {}

std::string PlaceLowerer::frameFunctionName() const {
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

llvm::Constant *PlaceLowerer::frameDescriptor(llvm::SMLoc loc, const std::string *name) {
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
      auto [it, inserted] = filePaths_.try_emplace(id);
      if (inserted && id.starts_with("tz:")) {
        it->second = id.str();
      } else if (inserted) {
        llvm::SmallString<256> path(id);
        llvm::sys::fs::make_absolute(path);
        llvm::sys::path::remove_dots(path, true);
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
  auto *global = new llvm::GlobalVariable(*lowering_.module_, type, true,
                                          llvm::GlobalValue::PrivateLinkage, init, "tz.frame");
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  frames_[key] = global;

  return global;
}

}
