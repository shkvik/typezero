#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/stream/chunk_types/chunk_types_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class ChunkTypes {
public:
  using ResultHookParamsMatch = Result<const FunctionDecl *, ChunkTypesHookParamsError>;

  explicit ChunkTypes(SemanticState &owner);

  bool isStreamClass(const ClassDecl *cls) const;
  StreamChunks streamChunksOf(const ClassDecl *cls) const;
  std::optional<std::vector<const Type *>> streamHookParams(const FunctionDecl *fn);
  bool isStreamHook(const FunctionDecl *fn, bool linked) const;
  ResultHookParamsMatch hookParams(const FunctionDecl *fn,
                                   llvm::ArrayRef<FunctionDecl *> inherited) const;
  void addStreamBases(ClassDecl *inst, llvm::SMLoc loc);
  const Type *inferredChunkType(const std::string &site, llvm::SMLoc loc);
  bool recordStreamChunk(const Type *chunk, const Type *value, llvm::SMLoc loc);

private:
  SemanticState &state;
};

}
