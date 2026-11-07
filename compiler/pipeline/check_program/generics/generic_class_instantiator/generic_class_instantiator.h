#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/generics/generic_class_instantiator/generic_class_instantiator_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class GenericClassInstantiator {
public:
  using ResultClassMade = Result<ClassDecl *, GenericClassInstantiatorInstantiateClassError>;

  explicit GenericClassInstantiator(CheckState &owner);

  ResultClassMade instantiateClass(ClassDecl *generic, std::vector<const Type *> args,
                                   llvm::SMLoc use);
  void fillInstance(ClassDecl *inst);
  void linkInstance(ClassDecl *inst);
  void declareInstanceMember(ClassDecl *inst, FunctionDecl *fn, FunctionDecl *generic);
  void markOverridden(FunctionDecl *fn);

private:
  static std::string typeNames(llvm::ArrayRef<const Type *> types);

  CheckState &state;
};

}
