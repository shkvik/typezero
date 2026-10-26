#include "compiler/library/result/err_sites/err_sites.h"
#include "compiler/lower/values/string_lowerer/string_lowerer.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

namespace tz {

namespace {

namespace path = llvm::sys::path;

std::string absolute(llvm::StringRef file) {
  llvm::SmallString<256> full(file);
  llvm::sys::fs::make_absolute(full);
  path::remove_dots(full, true);
  path::native(full);

  return std::string(full);
}

}

ErrSites::ErrSites(Lowering &lowering) : lowering_(lowering) {}

llvm::Type *ErrSites::type() { return lowering_.builder_.getPtrTy(); }

llvm::Constant *ErrSites::none() {
  return llvm::ConstantPointerNull::get(lowering_.builder_.getPtrTy());
}

std::string ErrSites::functionName() const {
  const FunctionDecl *fn = lowering_.currentFn_;
  while (fn && fn->isExpression && fn->source.empty()) {
    fn = fn->parent;
  }
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

std::string ErrSites::fileName(unsigned buffer) {
  if (auto found = files_.find(buffer); found != files_.end()) {
    return found->second;
  }
  const llvm::SourceMgr &sources = *lowering_.sourceMgr_;
  if (!entryKnown_) {
    entryKnown_ = true;
    if (sources.getNumBuffers() > 0) {
      entryDir_ =
          path::parent_path(absolute(sources.getMemoryBuffer(1)->getBufferIdentifier())).str();
    }
  }
  llvm::StringRef id = sources.getMemoryBuffer(buffer)->getBufferIdentifier();
  std::string name;
  if (id.starts_with("tz:") || id.starts_with("node:")) {
    name = id.str();
  } else {
    name = absolute(id);
    llvm::StringRef rest = name;
    if (!entryDir_.empty() && rest.consume_front(entryDir_) && !rest.empty() &&
        path::is_separator(rest.front())) {
      name = path::convert_to_slash(rest.drop_front());
    }
  }
  files_[buffer] = name;

  return name;
}

llvm::Constant *ErrSites::site(llvm::SMLoc loc) {
  if (!lowering_.sourceMgr_ || !loc.isValid()) {
    return none();
  }
  unsigned buffer = lowering_.sourceMgr_->FindBufferContainingLoc(loc);
  if (!buffer) {
    return none();
  }
  unsigned line = lowering_.sourceMgr_->getLineAndColumn(loc, buffer).first;
  std::string place = fileName(buffer) + ":" + std::to_string(line);
  std::string function = functionName();
  std::string text = function.empty() ? place : function + " (" + place + ")";
  llvm::Constant *&global = sites_[text];
  if (global) {
    return global;
  }
  llvm::Type *ptr = lowering_.builder_.getPtrTy(), *i32 = lowering_.builder_.getInt32Ty();
  auto *structType = llvm::StructType::get(lowering_.ctx_, {ptr, i32});
  llvm::Constant *init =
      llvm::ConstantStruct::get(structType, {lowering_.stringLowerer().utf8Constant(text),
                                             lowering_.builder_.getInt32(uint32_t(text.size()))});
  auto *variable = new llvm::GlobalVariable(*lowering_.module_, structType, true,
                                            llvm::GlobalValue::PrivateLinkage, init, "tz.site");
  variable->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  global = variable;

  return global;
}

}
