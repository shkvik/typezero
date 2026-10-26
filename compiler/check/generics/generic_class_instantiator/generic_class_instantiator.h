#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class GenericClassInstantiator {
public:
  explicit GenericClassInstantiator(Checker &checker);

  ClassDecl *instantiateClass(ClassDecl *generic, std::vector<const Type *> args, llvm::SMLoc loc);
  void fillInstance(ClassDecl *inst);
  void linkInstance(ClassDecl *inst);
  void declareInstanceMember(ClassDecl *inst, FunctionDecl *fn, FunctionDecl *generic);
  void markOverridden(FunctionDecl *fn);

private:
  Checker &checker_;
  llvm::SmallPtrSet<const ClassDecl *, 2> endless_;
  llvm::DenseMap<const ClassDecl *, Checker::Instance *> classChecks_;
};

}
