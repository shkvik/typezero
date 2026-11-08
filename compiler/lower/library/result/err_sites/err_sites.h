#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class ErrSites {
public:
  explicit ErrSites(Lowering &owner);

  llvm::Constant *site(llvm::SMLoc loc);
  llvm::Constant *none();
  bool inLibrary(llvm::SMLoc loc);
  llvm::Value *stamp(llvm::Value *result, const Type *type, llvm::SMLoc loc);
  llvm::Type *type();

private:
  static std::string absolute(llvm::StringRef file);

  std::string functionName() const;
  std::string fileName(unsigned buffer);

  Lowering &lowering;
  llvm::StringMap<llvm::Constant *> sites;
  llvm::DenseMap<unsigned, std::string> files;
  std::string entryDir;
  bool entryKnown = false;
};

}
