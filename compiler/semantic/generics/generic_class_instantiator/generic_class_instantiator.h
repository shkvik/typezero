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

  struct MemberTypes {
    ClassDecl *inst = nullptr;
    std::vector<const Type *> own;
    std::vector<const Type *> mine;
  };

  FuncSymbol *makeInstanceSymbol(FunctionDecl *fn, FunctionDecl *generic);
  std::vector<const Type *> iteratedParams(ClassDecl *inst, llvm::ArrayRef<const Type *> params);
  const Type *substituted(const MemberTypes &types, const Type *t);
  const Type *substitutedValue(const MemberTypes &types, const Type *t);
  void fillInstanceSignature(const MemberTypes &types, FunctionDecl *fn, FunctionDecl *generic,
                             FuncSymbol *sym);
  void fillInstanceParams(const MemberTypes &types, FunctionDecl *fn, const FunctionDecl *generic);
  void addInstanceBase(ClassDecl *inst, const Type *base);
  void linkInstanceBases(ClassDecl *inst);
  void fillInstanceFields(ClassDecl *inst);
  void declareInstanceMembers(ClassDecl *inst);
  void makeInstanceInit(ClassDecl *inst);
  void walkInstanceVariants(ClassDecl *inst);

  SemanticState &state;
  ChunkTypes *chunkTypes = nullptr;
  ClassBases *classBases = nullptr;
  ClassLinks *classLinks = nullptr;
  TypeParams *typeParams = nullptr;
  ClassDeclarer *classDeclarer = nullptr;
  GenericClassChecker *genericClassChecker = nullptr;
};

}
