#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// The instances of generic classes.
class GenericClassInstantiator {
public:
  explicit GenericClassInstantiator(Checker &checker);

  // `Box<int>`: the instance of `generic` for these type arguments (those
  // left out take their defaults), or the generic class itself for its own
  // type parameters. Null after an error, reported at `loc`.
  ClassDecl *instantiateClass(ClassDecl *generic, std::vector<const Type *> args, llvm::SMLoc loc);
  // Gives an instance the bases, fields and method signatures of its generic
  // class, which must have them by then; and once the classes are linked,
  // what linkClasses gives the others.
  void fillInstance(ClassDecl *inst);
  void linkInstance(ClassDecl *inst);
  // The FuncSymbol of `fn`, a member of an instance, from that of `generic`,
  // the member of the generic class it is a copy of.
  void declareInstanceMember(ClassDecl *inst, FunctionDecl *fn, FunctionDecl *generic);
  // The methods of an instance that are copies of `fn`, a method of a
  // generic class, and `fn` itself, marked as overridden by a class down
  // the chain: one instance's subclass makes the calls of each instance go
  // through its method table.
  void markOverridden(FunctionDecl *fn);

private:
  Checker &checker_;
  llvm::SmallPtrSet<const ClassDecl *, 2> endless_; // reported as such
  // The checks of the instances of generic classes, by class.
  llvm::DenseMap<const ClassDecl *, Checker::Instance *> classChecks_;
};

} // namespace tz
