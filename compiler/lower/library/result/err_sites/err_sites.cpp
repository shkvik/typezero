#include "compiler/lower/library/result/err_sites/err_sites.h"
#include "compiler/lower/library/result/result_lowerer/result_lowerer.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

namespace tz {

namespace {

namespace path = llvm::sys::path;

}

std::string ErrSites::absolute(llvm::StringRef file) {
  llvm::SmallString<256> full(file);
  llvm::sys::fs::make_absolute(full);
  path::remove_dots(full, true);
  path::native(full);

  return std::string(full);
}

ErrSites::ErrSites(Lowering &owner)
    : lowering(owner) {}

llvm::Type *ErrSites::type() {
  return this->lowering.builder.getPtrTy();
}

llvm::Constant *ErrSites::none() {
  return llvm::ConstantPointerNull::get(this->lowering.builder.getPtrTy());
}

std::string ErrSites::functionName() const {
  const FunctionDecl *fn = this->lowering.currentFn;
  while (fn && fn->isExpression() && fn->source.empty()) {
    fn = fn->parent;
  }
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

std::string ErrSites::fileName(unsigned buffer) {
  if (auto found = this->files.find(buffer); found != this->files.end()) {
    return found->second;
  }
  const llvm::SourceMgr &sources = *this->lowering.sourceMgr_;
  if (!this->entryKnown) {
    this->entryKnown = true;
    if (sources.getNumBuffers() > 0) {
      this->entryDir =
          path::parent_path(ErrSites::absolute(sources.getMemoryBuffer(1)->getBufferIdentifier()))
              .str();
    }
  }
  llvm::StringRef id = sources.getMemoryBuffer(buffer)->getBufferIdentifier();
  std::string name;
  if (id.starts_with("tz:") || id.starts_with("node:")) {
    name = id.str();
  } else {
    name = ErrSites::absolute(id);
    llvm::StringRef rest = name;
    if (!this->entryDir.empty() && rest.consume_front(this->entryDir) && !rest.empty() &&
        path::is_separator(rest.front())) {
      name = path::convert_to_slash(rest.drop_front());
    }
  }
  this->files[buffer] = name;

  return name;
}

bool ErrSites::inLibrary(llvm::SMLoc loc) {
  if (!this->lowering.sourceMgr_ || !loc.isValid()) {
    return false;
  }
  unsigned buffer = this->lowering.sourceMgr_->FindBufferContainingLoc(loc);
  if (!buffer) {
    return false;
  }
  llvm::StringRef id = this->lowering.sourceMgr_->getMemoryBuffer(buffer)->getBufferIdentifier();

  return id.starts_with("tz:") || id.starts_with("node:");
}

llvm::Value *ErrSites::stamp(llvm::Value *result, const Type *type, llvm::SMLoc loc) {
  if (type->errType()->isNever()) {
    return result;
  }
  Lowering::ResultLayout layout = this->lowering.resultLowerer().resultLayout(type);
  llvm::Value *old = this->lowering.builder.CreateExtractValue(result, layout.site, "site");
  llvm::Value *site = this->lowering.builder.CreateSelect(this->lowering.builder.CreateIsNull(old),
                                                          this->site(loc), old, "site");

  return this->lowering.builder.CreateInsertValue(result, site, layout.site);
}

llvm::Constant *ErrSites::site(llvm::SMLoc loc) {
  if (!this->lowering.sourceMgr_ || !loc.isValid() || this->inLibrary(loc)) {
    return this->none();
  }
  unsigned buffer = this->lowering.sourceMgr_->FindBufferContainingLoc(loc);
  if (!buffer) {
    return this->none();
  }
  unsigned line = this->lowering.sourceMgr_->getLineAndColumn(loc, buffer).first;
  std::string place = this->fileName(buffer) + ":" + std::to_string(line);
  std::string function = this->functionName();
  std::string text = function.empty() ? place : function + " (" + place + ")";
  llvm::Constant *&global = this->sites[text];
  if (global) {
    return global;
  }
  llvm::Type *ptr = this->lowering.builder.getPtrTy();
  llvm::Type *i32 = this->lowering.builder.getInt32Ty();
  auto *structType = llvm::StructType::get(this->lowering.ctx, {ptr, i32});
  llvm::Constant *init = llvm::ConstantStruct::get(
      structType, {this->lowering.stringLowerer().utf8Constant(text),
                   this->lowering.builder.getInt32(uint32_t(text.size()))});
  auto *variable = new llvm::GlobalVariable(*this->lowering.module_, structType, true,
                                            llvm::GlobalValue::PrivateLinkage, init, "tz.site");
  variable->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  global = variable;

  return global;
}

}
