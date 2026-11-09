#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/generics/generic_class_instantiator/generic_class_instantiator_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class ChunkTypes;
class ClassBases;
class ClassLinks;
class TypeParams;
class ClassDeclarer;
class GenericClassChecker;

class GenericClassInstantiator {
public:
  using ResultClassMade = Result<ClassDecl *, GenericClassInstantiatorInstantiateClassError>;

  GenericClassInstantiator(SemanticState &owner, SemanticParts &all);

  ResultClassMade instantiateClass(ClassDecl *generic, std::vector<const Type *> args,
                                   llvm::SMLoc use);
  void fillInstance(ClassDecl *inst);
  void linkInstance(ClassDecl *inst);
  void declareInstanceMember(ClassDecl *inst, FunctionDecl *fn, FunctionDecl *generic);
  void markOverridden(FunctionDecl *fn);

private:
  static std::string typeNames(llvm::ArrayRef<const Type *> types);

  SemanticState &state;
  ChunkTypes *chunkTypes = nullptr;
  ClassBases *classBases = nullptr;
  ClassLinks *classLinks = nullptr;
  TypeParams *typeParams = nullptr;
  ClassDeclarer *classDeclarer = nullptr;
  GenericClassChecker *genericClassChecker = nullptr;
};

}
