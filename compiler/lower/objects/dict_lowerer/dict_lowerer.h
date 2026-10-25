#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// Record, Map and Set: layouts, lookups, iteration and literals.
class DictLowerer {
public:
  explicit DictLowerer(CodeGen &codegen);

  const CodeGen::DictLayout &dictLayout(const Type *type);
  llvm::FunctionCallee dictFunction(llvm::StringRef name, llvm::Type *ret,
                                    llvm::ArrayRef<llvm::Type *> params);
  llvm::Value *dictField(llvm::Value *map, unsigned field);
  // The entry of a borrowed key in a borrowed map, or null; with `add`, the
  // entry made for it if there was none (the map keeps a copy of the key),
  // and `added` is set then, if given.
  llvm::Value *emitDictFind(llvm::Value *map, llvm::Value *key, const Type *keyType);
  llvm::Value *emitDictAdd(llvm::Value *map, llvm::Value *key, const Type *keyType,
                           llvm::Value **added = nullptr);
  llvm::Value *entryKey(llvm::Value *entry, const Type *type);
  llvm::Value *entryValue(llvm::Value *entry, const Type *type);
  // After the value of a Record's entry changed: process.env tells the
  // environment.
  void emitEntryStored(llvm::Value *map, llvm::Value *entry);
  // While a dictionary held in `slot` is iterated its entries stay where
  // they are; the scope ends the iteration.
  void beginDictIteration(llvm::Value *slot);
  // The iteration of the dictionary in `slot`: keys, values or entries as
  // the iterator builtin `mode` gives them; `position` is the slot of the
  // position, if it must be one that the runtime sees (see
  // emitPendingIterators).
  CodeGen::Producer dictProducer(llvm::Value *slot, const Type *type, Builtin mode,
                        llvm::Value *position = nullptr);
  // Calls `each` with each live entry of the dictionary in `slot`.
  void emitDictLoop(llvm::Value *slot, const Type *type, llvm::Value *position,
                    llvm::function_ref<void(llvm::Value *)> each);
  llvm::Value *emitDictForEach(CallExpr *call);
  llvm::Value *emitRecordLiteral(ObjectLitExpr *lit);

private:
  CodeGen &codegen_;
  std::map<const Type *, CodeGen::DictLayout> dictLayouts_; // stable while more are added
  llvm::StructType *mapTy_ = nullptr;              // TzMap
  llvm::StructType *mapDescTy_ = nullptr;          // TzMapLayout
};

} // namespace tz
