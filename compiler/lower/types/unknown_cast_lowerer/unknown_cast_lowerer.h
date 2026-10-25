#pragma once

#include "compiler/lower/codegen/codegen.h"

namespace tz {

// unknown as T: the tests of the classes a value may be.
class UnknownCastLowerer {
public:
  explicit UnknownCastLowerer(CodeGen &codegen);

  // `u`, an unknown (borrowed), as an owned value of `to`; control goes to
  // `fail`, with nothing owned, if it holds no such value.
  llvm::Value *emitUnknownAs(llvm::Value *u, const Type *to, llvm::BasicBlock *fail);
  // The classes whose objects are of the object type `type`: a class and
  // those down its chain, or those that implement an interface; and whether
  // the class number `id` is that of one of `classes` or of a class down the
  // chain of one.
  std::vector<const ClassDecl *> classesOfType(const Type *type);
  llvm::Value *classIdIn(llvm::Value *id, llvm::ArrayRef<const ClassDecl *> classes);
  // How well the unknown `u` (borrowed) fits a parameter of type `to`, an
  // i32, to choose among the variants of a method or a constructor when the
  // program runs (docs/overloads.md, section 6): 0 not at all, 1 only through
  // the check (an integral double for an int, anything for an unknown), 2 by
  // widening (an int for a double, an object for a class up its chain), 3
  // exactly.
  llvm::Value *emitUnknownFit(llvm::Value *u, const Type *to);

private:
  CodeGen &codegen_;
};

} // namespace tz
