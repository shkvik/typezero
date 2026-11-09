#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/stream/set_encoding_checker/set_encoding_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class ChunkTypes;
class FieldFacts;
class StreamSources;
class EmitterChecker;
class ProcessChecker;
class GenericClassInstantiator;

class SetEncodingChecker {
public:
  SetEncodingChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkSetEncoding(CallExpr *call, MemberExpr *member, ClassDecl *cls);
  const Type *encodedStreamType(ClassDecl *cls, llvm::SMLoc loc);

private:
  using ResultStdinIsPlaced = Result<const Module *, SetEncodingCheckerStdinPlaceError>;
  using ResultDecodedChunks = Result<const Type *, SetEncodingCheckerDecodedChunksError>;

  ResultStdinIsPlaced stdinPlace() const;
  static ResultDecodedChunks decodedChunks(const Type *readable, const Type *receiver);

  SemanticState &state;
  ChunkTypes *chunkTypes = nullptr;
  FieldFacts *fieldFacts = nullptr;
  StreamSources *streamSources = nullptr;
  EmitterChecker *emitterChecker = nullptr;
  ProcessChecker *processChecker = nullptr;
  GenericClassInstantiator *genericClassInstantiator = nullptr;
};

}
