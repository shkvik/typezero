#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class ErrSites {
public:
  explicit ErrSites(Lowering &lowering);

  llvm::Constant *site(llvm::SMLoc loc);
  llvm::Constant *none();
  bool inLibrary(llvm::SMLoc loc);
  llvm::Value *stamp(llvm::Value *result, const Type *type, llvm::SMLoc loc);
  llvm::Type *type();

private:
  std::string functionName() const;
  std::string fileName(unsigned buffer);

  Lowering &lowering_;
  llvm::StringMap<llvm::Constant *> sites_;
  llvm::DenseMap<unsigned, std::string> files_;
  std::string entryDir_;
  bool entryKnown_ = false;
};

}
