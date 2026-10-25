#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// Groups of objects in cycles: their code.
class GroupLowerer {
public:
  explicit GroupLowerer(CodeGen &codegen);

  llvm::FunctionCallee groupFunction(llvm::StringRef name, llvm::Type *ret,
                                     llvm::ArrayRef<llvm::Type *> params);
  // A new member of `size` bytes, of a group of its own, counted once.
  llvm::Value *allocMember(uint64_t size, const llvm::Twine &name);
  // A fresh object of `type` whose fields the code filled with plain stores:
  // if it is a member, what they hold joins its group.
  void adoptFields(llvm::Value *object, const Type *type);
  // A layout for the environments and boxes that are members, which the
  // runtime reads their drop function and name from.
  llvm::Constant *memberDescriptor(llvm::Function *drop, llvm::StringRef name);
  // A new array of `arrayType` (`tz_arr_new`, or a member).
  llvm::Value *newArray(const Type *arrayType, llvm::Value *length, llvm::Value *capacity,
                        const llvm::Twine &name = "array");
  llvm::Value *newArray(const Type *arrayType, llvm::Value *length, llvm::Value *capacity,
                        bool member, const llvm::Twine &name);
  // Counting of an object whose static type is not known here: through its
  // group if it is a member (when the program has any).
  void retainObject(llvm::Value *object);
  // The group-aware form of a release function of objects: members go
  // through their group.
  llvm::Function *groupAwareRelease(llvm::Function *plain);
  llvm::Constant *groupSite();
  // An object made at `site` that the compiler found is never stored.
  void noteUnstored(llvm::Value *object, const Expr *site);

private:
  CodeGen &codegen_;
  std::map<std::pair<llvm::Constant *, std::string>, llvm::Constant *> groupSites_;
  llvm::DenseMap<llvm::Function *, llvm::Function *> groupReleases_;
};

} // namespace tz
