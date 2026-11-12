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
  bool isStreamHook(const FunctionDecl *functionDecl) const;
  bool isLinkedStreamHook(const FunctionDecl *functionDecl) const;
  ResultHookParamsMatch checkHookParams(const FunctionDecl *functionDecl,
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
  const Type *libraryAliasOf(LibraryType libraryType, const Type *arg, llvm::SMLoc loc);
  bool isTypedLibraryStream(const ClassDecl *classDecl) const;
  void addStreamChunk(StreamChunks &chunks, const ClassDecl *classDecl) const;
  const FuncSymbol *libraryHook(llvm::ArrayRef<FunctionDecl *> inherited) const;
  bool isUntypedParam(const Type *type) const;

  SemanticState &state;
  Prelude *prelude = nullptr;
  ClassBases *classBases = nullptr;
  AliasInstantiator *aliasInstantiator = nullptr;
  GenericClassInstantiator *genericClassInstantiator = nullptr;
};

}
