#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/generics/generic_class_instantiator/generic_class_instantiator_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class EventMaps;
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

  void fillInstance(ClassDecl *instance);
  void linkInstance(ClassDecl *instance);
  void markOverridden(FunctionDecl *functionDecl);
  ResultClassMade instantiateClass(ClassDecl *generic, std::vector<const Type *> args,
                                   llvm::SMLoc use);
  void declareInstanceMember(ClassDecl *instance, FunctionDecl *functionDecl,
                             FunctionDecl *generic);

private:
  struct MemberTypes {
    ClassDecl *instance;
    std::vector<const Type *> genericParams;
    std::vector<const Type *> instanceParams;
  };

  using ResultCanBeMade = Result<void, GenericClassInstantiatorInstantiateClassError>;
  using ResultArgsKnown =
      Result<std::vector<const Type *>, GenericClassInstantiatorInstantiateClassError>;

  ResultCanBeMade checkChainBounded(const ClassDecl *generic);
  ResultCanBeMade checkArguments(const ClassDecl *generic, const std::vector<const Type *> &args);
  ResultCanBeMade checkDefaultPresent(const ClassDecl *generic, const Type *defaultType) const;
  ResultCanBeMade checkArgumentCount(const ClassDecl *generic,
                                     llvm::ArrayRef<const Type *> args) const;
  ResultArgsKnown argumentsWithDefaults(const ClassDecl *generic, std::vector<const Type *> args);
  ResultCanBeMade checkEventMap(const ClassDecl *generic, llvm::ArrayRef<const Type *> args,
                                llvm::SMLoc loc);
  ResultCanBeMade checkArgument(const ClassDecl *generic, const std::vector<const Type *> &args,
                                size_t index);

  void makeInstanceInit(ClassDecl *instance);
  void fillOrPostpone(ClassDecl *instance);
  void linkInstanceBases(ClassDecl *instance);
  void linkInstanceParent(ClassDecl *instance);
  void fillInstanceFields(ClassDecl *instance);
  void walkInstanceVariants(ClassDecl *instance);
  void declareInstanceMembers(ClassDecl *instance);
  void setOverridden(FunctionDecl *functionDecl) const;
  bool isStreamClass(const ClassDecl *classDecl) const;
  bool isEmitterClass(const ClassDecl *classDecl) const;
  void rememberCheck(ClassDecl *instance, llvm::SMLoc use);
  void addInstanceBase(ClassDecl *instance, const Type *base);
  bool isMentioningParam(llvm::ArrayRef<const Type *> args) const;
  void walkOrPostpone(const ClassDecl *instance, llvm::SMLoc loc);
  void registerInstance(ClassDecl *generic, ClassDecl *instance);
  const Type *iteratedParam(ClassDecl *instance, const Type *param);
  const Type *substituted(const MemberTypes &types, const Type *type);
  std::string constraintText(const ClassDecl *generic, size_t index) const;
  const Type *substitutedValue(const MemberTypes &types, const Type *type);
  void reuseInstance(const ClassDecl *known, llvm::SMLoc use, llvm::SMLoc loc);
  ClassDecl *makeInstance(ClassDecl *generic, const std::vector<const Type *> &args);
  FuncSymbol *makeInstanceSymbol(FunctionDecl *functionDecl, FunctionDecl *generic);
  void markInstanceCopies(const ClassDecl *instance, const FunctionDecl *generic) const;
  std::string instanceName(const ClassDecl *generic, llvm::ArrayRef<const Type *> args) const;
  void fillInstanceField(ClassDecl *instance, ClassField &field, const ClassField &genericField);
  std::vector<const Type *> iteratedParams(ClassDecl *instance,
                                           llvm::ArrayRef<const Type *> params);
  void fillInstanceParams(const MemberTypes &types, FunctionDecl *functionDecl,
                          const FunctionDecl *generic);
  void fillInstanceSignature(const MemberTypes &types, FunctionDecl *functionDecl,
                             FunctionDecl *generic, FuncSymbol *function);

  SemanticState &state;
  EventMaps *eventMaps = nullptr;
  ChunkTypes *chunkTypes = nullptr;
  ClassBases *classBases = nullptr;
  ClassLinks *classLinks = nullptr;
  TypeParams *typeParams = nullptr;
  ClassDeclarer *classDeclarer = nullptr;
  GenericClassChecker *genericClassChecker = nullptr;
};

}
