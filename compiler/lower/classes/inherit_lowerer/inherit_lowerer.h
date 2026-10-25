#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// Tables of methods and of interfaces, and the calls through them.
class InheritLowerer {
public:
  explicit InheritLowerer(CodeGen &codegen);

  void prepareDispatch(SourceFile &file);
  // How code reaches member `name` of objects of `type`, if through a table;
  // `signature` picks one of a method with several signatures.
  const CodeGen::InterfaceMember *dispatchedMember(const Type *type, llvm::StringRef name,
                                          int signature = -1);
  // The address of entry `entry` of the table of `iface` in `object`'s
  // descriptor.
  llvm::Value *tableEntry(llvm::Value *object, const Type *iface, unsigned entry);
  // The itables of the objects of `type`: null if it implements no interface
  // that needs them.
  llvm::Constant *itablesOf(const Type *type);
  llvm::Constant *itable(const Type *type, const Type *iface);
  // The code a method of an interface runs for an object of a type that
  // holds a function in the field, which it calls; and its info.
  std::pair<llvm::Function *, llvm::Constant *> fieldStub(const Type *type, const Field &member);
  // The method of class `cls` (or up its chain) that implements a member of
  // type `fnType`.
  FunctionDecl *implementingMethod(const ClassDecl *cls, llvm::StringRef name, const Type *fnType);
  // `o.m(args)` through an interface.
  llvm::Value *emitInterfaceCall(CallExpr *call);
  llvm::Value *vtableEntry(llvm::Value *object, int slot, bool info);
  // The layout whose fields an object type puts first: of the class it
  // extends, or of an interface it implements; null for none.
  const Type *layoutPrefix(const Type *type) const;

private:
  CodeGen &codegen_;
  std::map<const Type *, std::vector<const Type *>> tablesOf_;
  unsigned colors_ = 0;
  std::map<const Type *, llvm::Constant *> itables_;
  std::map<std::pair<const Type *, const Type *>, llvm::Constant *> itableConstants_;
  std::map<std::pair<const Type *, std::string>, std::pair<llvm::Function *, llvm::Constant *>>
      fieldStubs_;
};

} // namespace tz
