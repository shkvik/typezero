#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

class InheritLowerer {
public:
  explicit InheritLowerer(Lowering &owner);

  void prepareDispatch(SourceFile &file);
  const Lowering::InterfaceMember *dispatchedMember(const Type *type, llvm::StringRef name,
                                                    int signature = -1);
  llvm::Value *tableEntry(llvm::Value *object, const Type *iface, unsigned entry);
  llvm::Constant *itablesOf(const Type *type);
  llvm::Constant *itable(const Type *type, const Type *iface);
  std::pair<llvm::Function *, llvm::Constant *> fieldStub(const Type *type, const Field &member);
  FunctionDecl *implementingMethod(const ClassDecl *cls, llvm::StringRef name, const Type *fnType);
  llvm::Value *emitInterfaceCall(CallExpr *call);
  llvm::Value *vtableEntry(llvm::Value *object, int slot, bool info);
  const Type *layoutPrefix(const Type *type) const;

private:
  static llvm::LoadInst *invariant(llvm::LoadInst *load);
  static std::string typeName(const Type *type);

  Lowering &lowering;
  std::map<const Type *, std::vector<const Type *>> tablesOf;
  unsigned colors = 0;
  std::map<const Type *, llvm::Constant *> itables;
  std::map<std::pair<const Type *, const Type *>, llvm::Constant *> itableConstants;
  std::map<std::pair<const Type *, std::string>, std::pair<llvm::Function *, llvm::Constant *>>
      fieldStubs;
};

}
