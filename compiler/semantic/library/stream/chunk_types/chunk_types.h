#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/stream/chunk_types/chunk_types_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class Prelude;
class ClassBases;
class AliasInstantiator;
class GenericClassInstantiator;

class ChunkTypes {
public:
  using ResultHookParamsMatch = Result<const FunctionDecl *, ChunkTypesHookParamsError>;

  ChunkTypes(SemanticState &owner, SemanticParts &all);

  bool isStreamClass(const ClassDecl *classDecl) const;
  StreamChunks streamChunksOf(const ClassDecl *classDecl) const;
  std::optional<std::vector<const Type *>> streamHookParams(const FunctionDecl *functionDecl);
  bool isStreamHook(const FunctionDecl *functionDecl, bool linked) const;
  ResultHookParamsMatch hookParams(const FunctionDecl *functionDecl,
                                   llvm::ArrayRef<FunctionDecl *> inherited) const;
  void addStreamBases(ClassDecl *instance, llvm::SMLoc loc);

private:
  struct StreamSides {
    const Type *readable;
    const Type *writable;
    const Type *error;
    bool transform;
  };

  const ClassDecl *libraryBase(const ClassDecl *classDecl) const;
  std::optional<StreamSides> streamSides(const ClassDecl *lib);
  std::vector<const Type *> hookParamTypes(const FunctionDecl *functionDecl,
                                           const StreamSides &sides);
  std::vector<const Type *> destroyParamTypes();
  std::vector<const Type *> writevParamTypes(const FunctionDecl *functionDecl,
                                             const StreamSides &sides, const Type *callback);
  const Type *libraryAliasOf(llvm::StringRef name, const Type *arg, llvm::SMLoc loc);

  SemanticState &state;
  Prelude *prelude = nullptr;
  ClassBases *classBases = nullptr;
  AliasInstantiator *aliasInstantiator = nullptr;
  GenericClassInstantiator *genericClassInstantiator = nullptr;
};

}
