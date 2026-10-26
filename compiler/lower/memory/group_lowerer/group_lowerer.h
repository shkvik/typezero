#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class GroupLowerer {
public:
  explicit GroupLowerer(Lowering &lowering);

  llvm::FunctionCallee groupFunction(llvm::StringRef name, llvm::Type *ret,
                                     llvm::ArrayRef<llvm::Type *> params);
  llvm::Value *allocMember(uint64_t size, const llvm::Twine &name);
  void adoptFields(llvm::Value *object, const Type *type);
  llvm::Constant *memberDescriptor(llvm::Function *drop, llvm::StringRef name);
  llvm::Value *newArray(const Type *arrayType, llvm::Value *length, llvm::Value *capacity,
                        const llvm::Twine &name = "array");
  llvm::Value *newArray(const Type *arrayType, llvm::Value *length, llvm::Value *capacity,
                        bool member, const llvm::Twine &name);
  void retainObject(llvm::Value *object);
  llvm::Function *groupAwareRelease(llvm::Function *plain);
  llvm::Constant *groupSite();
  void noteUnstored(llvm::Value *object, const Expr *site);

private:
  Lowering &lowering_;
  std::map<std::pair<llvm::Constant *, std::string>, llvm::Constant *> groupSites_;
  llvm::DenseMap<llvm::Function *, llvm::Function *> groupReleases_;
};

}
