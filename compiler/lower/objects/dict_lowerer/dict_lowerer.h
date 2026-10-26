#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class DictLowerer {
public:
  explicit DictLowerer(Lowering &lowering);

  const Lowering::DictLayout &dictLayout(const Type *type);
  llvm::FunctionCallee dictFunction(llvm::StringRef name, llvm::Type *ret,
                                    llvm::ArrayRef<llvm::Type *> params);
  llvm::Value *dictField(llvm::Value *map, unsigned field);
  llvm::Value *emitDictFind(llvm::Value *map, llvm::Value *key, const Type *keyType);
  llvm::Value *emitDictAdd(llvm::Value *map, llvm::Value *key, const Type *keyType,
                           llvm::Value **added = nullptr);
  llvm::Value *entryKey(llvm::Value *entry, const Type *type);
  llvm::Value *entryValue(llvm::Value *entry, const Type *type);
  void emitEntryStored(llvm::Value *map, llvm::Value *entry);
  void beginDictIteration(llvm::Value *slot);
  Lowering::Producer dictProducer(llvm::Value *slot, const Type *type, Builtin mode,
                                  llvm::Value *position = nullptr);
  void emitDictLoop(llvm::Value *slot, const Type *type, llvm::Value *position,
                    llvm::function_ref<void(llvm::Value *)> each);
  llvm::Value *emitDictForEach(CallExpr *call);
  llvm::Value *emitRecordLiteral(ObjectLitExpr *lit);

private:
  Lowering &lowering_;
  std::map<const Type *, Lowering::DictLayout> dictLayouts_;
  llvm::StructType *mapTy_ = nullptr;
  llvm::StructType *mapDescTy_ = nullptr;
};

}
