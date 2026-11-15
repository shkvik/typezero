#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class ForeignLowerer {
public:
  explicit ForeignLowerer(Lowering &owner);

  void emitForeign(FunctionDecl *fn);

private:
  llvm::Type *foreignType(const Type *type);
  llvm::Value *foreignArgument(llvm::Value *value, const Type *type,
                               std::vector<llvm::Value *> &written);
  llvm::Value *outSlot(llvm::Value *value, const Type *type, std::vector<llvm::Value *> &written);
  llvm::FunctionCallee foreignFunction(const FunctionDecl *fn, llvm::FunctionType *type,
                                       llvm::AttributeList attributes);
  llvm::AttributeList foreignAttributes(const FuncSymbol *sym);
  llvm::Attribute::AttrKind extensionOf(const Type *type) const;
  void returnForeign(const FunctionDecl *fn, llvm::Value *result);
  void checkNotNull(const FunctionDecl *fn, llvm::Value *result);
  llvm::Value *copyPresent(llvm::Value *result);

  Lowering &lowering;
};

}
