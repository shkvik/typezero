#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class GenericClassInstantiator {
public:
  explicit GenericClassInstantiator(Checker &owner);

  ClassDecl *instantiateClass(ClassDecl *generic, std::vector<const Type *> args, llvm::SMLoc loc);
  void fillInstance(ClassDecl *inst);
  void linkInstance(ClassDecl *inst);
  void declareInstanceMember(ClassDecl *inst, FunctionDecl *fn, FunctionDecl *generic);
  void markOverridden(FunctionDecl *fn);

private:
  static std::string typeNames(llvm::ArrayRef<const Type *> types);

  Checker &checker;
  llvm::SmallPtrSet<const ClassDecl *, 2> endless;
  llvm::DenseMap<const ClassDecl *, Checker::Instance *> classChecks;
};

}
