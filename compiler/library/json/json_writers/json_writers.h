#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class JsonWriters {
public:
  explicit JsonWriters(Lowering &owner);

  bool writable(const Type *type);
  llvm::Value *emitStringify(llvm::Value *value, const Type *type);
  void finish();

private:
  struct Pending {
    const Type *type;
    llvm::Function *fn;
    bool array;
  };

  static bool skipped(const Type *type);
  static bool plainName(llvm::StringRef name);
  bool plainObject(const Type *type) const;
  bool check(const Type *type, llvm::SmallPtrSetImpl<const Type *> &open);
  llvm::FunctionCallee runtime(llvm::StringRef name, llvm::Type *ret,
                               llvm::ArrayRef<llvm::Type *> params);
  llvm::Function *writer(const Type *type, bool array);
  void writeText(llvm::Value *out, llvm::StringRef text);
  void writeRaw(llvm::Value *out, llvm::Value *text, llvm::Value *length);
  void writeValue(llvm::Value *out, const Type *type, llvm::Value *value);
  void emitArrayBody(const Type *element, llvm::Function *fn);
  void emitObjectBody(const Type *type, llvm::Function *fn);
  void emitFields(const Type *type, llvm::ArrayRef<unsigned> keys, llvm::Value *out,
                  llvm::Value *object, llvm::Value *written);

  Lowering &lowering;
  llvm::DenseMap<const Type *, bool> writableTypes;
  llvm::DenseMap<const Type *, llvm::Function *> objects;
  llvm::DenseMap<const Type *, llvm::Function *> arrays;
  std::vector<Pending> pending;
};

}
