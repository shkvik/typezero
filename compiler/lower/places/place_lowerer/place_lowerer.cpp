#include "compiler/lower/places/place_lowerer/place_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

namespace tz {

PlaceLowerer::PlaceLowerer(Lowering &owner)
    : lowering(owner) {}

std::string PlaceLowerer::frameFunctionName() const {
  const FunctionDecl *fn = this->lowering.currentFn_;
  if (!fn) {
    return "";
  }
  if (fn->cls() && !fn->cls()->literal()) {
    if (fn->isConstructor() || fn == fn->cls()->init) {
      return "new " + fn->cls()->name.str();
    }
    return (fn->cls()->name + "." + fn->name).str();
  }

  return fn->name.str();
}

llvm::Constant *PlaceLowerer::frameDescriptor(llvm::SMLoc loc, const std::string *name) {
  std::string function = name ? *name : this->frameFunctionName();
  auto key = std::make_pair(function, loc.getPointer());
  auto found = this->frames.find(key);
  if (found != this->frames.end()) {
    return found->second;
  }
  std::string file;
  unsigned line = 0;
  unsigned column = 0;
  if (this->lowering.sourceMgr_ && loc.isValid()) {
    unsigned buffer = this->lowering.sourceMgr_->FindBufferContainingLoc(loc);
    if (buffer) {
      std::tie(line, column) = this->lowering.sourceMgr_->getLineAndColumn(loc, buffer);
      llvm::StringRef id =
          this->lowering.sourceMgr_->getMemoryBuffer(buffer)->getBufferIdentifier();
      auto [it, inserted] = this->filePaths.try_emplace(id);
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
  llvm::Type *ptr = this->lowering.builder_.getPtrTy();
  llvm::Type *i32 = this->lowering.builder_.getInt32Ty();
  auto *type = llvm::StructType::get(this->lowering.ctx_, {ptr, i32, i32, ptr, i32, i32});
  llvm::Constant *init =
      llvm::ConstantStruct::get(type, {this->lowering.stringLowerer().utf8Constant(function),
                                       this->lowering.builder_.getInt32(uint32_t(function.size())),
                                       this->lowering.builder_.getInt32(line),
                                       this->lowering.stringLowerer().utf8Constant(file),
                                       this->lowering.builder_.getInt32(uint32_t(file.size())),
                                       this->lowering.builder_.getInt32(column)});
  auto *global = new llvm::GlobalVariable(*this->lowering.module_, type, true,
                                          llvm::GlobalValue::PrivateLinkage, init, "tz.frame");
  global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  this->frames[key] = global;

  return global;
}

}
