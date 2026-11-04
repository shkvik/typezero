#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class ClassInfoLowerer {
public:
  explicit ClassInfoLowerer(Lowering &lowering);

  std::vector<const FunctionDecl *> reflectVariants(const ClassDecl *cls, llvm::StringRef name);
  llvm::StructType *classInfoType();
  llvm::Constant *classInfoGlobal(const ClassDecl *cls);
  void emitConstructThunk(const ClassDecl *cls, llvm::Function *fn);
  struct FillErrors {
    const Type *type = nullptr;
    const Type *unknownField = nullptr;
    const Type *badClass = nullptr;
  };
  void emitFillThunk(const ClassDecl *cls, llvm::Function *fn, const FillErrors &errors);
  llvm::Function *fillMaker(const Type *variant);
  llvm::Constant *reflectAdapterInfo(const FunctionDecl *fn, const Type *&type);

private:
  struct FillSite {
    llvm::Value *cls;
    llvm::Value *clsLength;
    llvm::Value *field;
    llvm::Value *fieldLength;
    llvm::Function *maker;
  };
  llvm::Value *fillValue(llvm::Value *u, const Type *type, const FillSite &site,
                         llvm::BasicBlock *mismatch, llvm::BasicBlock *failed);
  llvm::Function *fillElement(const Type *element, const FillSite &site);
  llvm::Constant *fillNames(llvm::ArrayRef<std::string> names, llvm::StringRef label);
  void emitFillElements();

  Lowering &lowering_;
  llvm::DenseMap<const Type *, llvm::Function *> fillElements_;
  std::vector<std::pair<llvm::Function *, const Type *>> pendingElements_;
  std::vector<FillSite> pendingSites_;
  llvm::DenseMap<const FunctionDecl *, llvm::Constant *> reflectAdapters_;
  llvm::StructType *classInfoTy_ = nullptr;
};

}
