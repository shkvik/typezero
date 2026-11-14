#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/generics_v2/classes/generics_classes_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <string>
#include <vector>

namespace tz {

class ClassInstances {
public:
  explicit ClassInstances(SemanticState &owner);

  void linkInstance(const LinkInstance &task);
  void markOverridden(const MarkOverridden &task);
  void walkOrPostpone(const WalkOrPostpone &task);
  void afterInstanceWalk(const AfterInstanceWalk &task);
  void finishClassInstance(const FinishClassInstance &task);
  void nextInstanceDefault(const NextInstanceDefault &task);
  void afterInstanceDefault(const AfterInstanceDefault &task);
  void instantiateClassType(const InstantiateClassType &task);
  void nextInstanceArgument(const NextInstanceArgument &task);
  void afterInstanceArgument(const AfterInstanceArgument &task);
  void afterInstanceTypeParams(const AfterInstanceTypeParams &task);

private:
  using ResultCanBeMade = Result<void, GenericsClassesRegistryInstantiateClassError>;

  void fillOrPostpone(ClassDecl *instance);
  void finishInstanceChecks(ClassInstanceDraft draft);
  void setOverridden(FunctionDecl *functionDecl) const;
  void rememberCheck(ClassDecl *instance, llvm::SMLoc use);
  void registerInstance(ClassDecl *generic, ClassDecl *instance);
  void reuseInstance(const ClassDecl *known, llvm::SMLoc use, llvm::SMLoc loc);
  void markInstanceCopies(const ClassDecl *instance, const FunctionDecl *generic) const;
  void failInstance(const ClassInstanceDraft &draft,
                    const GenericsClassesRegistryInstantiateClassError &error);


  bool isStreamClass(const ClassDecl *classDecl) const;
  bool isEmitterClass(const ClassDecl *classDecl) const;
  bool isMentioningParam(llvm::ArrayRef<const Type *> args) const;


  std::string constraintText(const ClassDecl *generic, size_t index) const;
  std::string instanceName(const ClassDecl *generic, llvm::ArrayRef<const Type *> args) const;


  ResultCanBeMade checkChainBounded(const ClassDecl *generic);
  ResultCanBeMade checkDefaultPresent(const ClassDecl *generic, const Type *defaultType) const;
  ResultCanBeMade checkArgumentCount(const ClassDecl *generic,
                                     llvm::ArrayRef<const Type *> args) const;
  ResultCanBeMade checkArgumentKind(const ClassDecl *generic, const std::vector<const Type *> &args,
                                    size_t index) const;


  ClassDecl *makeInstance(ClassDecl *generic, const std::vector<const Type *> &args);

  SemanticState &state;
};

}
