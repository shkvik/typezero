#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

class ChunkTypes {
public:
  explicit ChunkTypes(Checker &checker);

  bool isStreamClass(const ClassDecl *cls) const;
  Checker::StreamChunks streamChunksOf(const ClassDecl *cls) const;
  std::optional<std::vector<const Type *>> streamHookParams(const FunctionDecl *fn);
  bool isStreamHook(const FunctionDecl *fn, bool linked) const;
  void reportStreamHook(const FunctionDecl *fn, llvm::ArrayRef<FunctionDecl *> inherited);
  void addStreamBases(ClassDecl *inst, llvm::SMLoc loc);
  const Type *inferredChunkType(const std::string &site, llvm::SMLoc loc);
  bool recordStreamChunk(const Type *chunk, const Type *value, llvm::SMLoc loc);
  void noteStreamCallback(CallExpr *call, const VarSymbol *var);

private:
  Checker &checker_;
};

}
