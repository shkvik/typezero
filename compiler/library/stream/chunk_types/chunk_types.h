#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// The types of the chunks of streams, from their classes and hooks.
class ChunkTypes {
public:
  explicit ChunkTypes(Sema &sema);

  // Streams (sema_stream.cpp, docs/streams.md): node:stream, which tz has
  // written in tz (library/stream/stream.tz), and what the compiler adds to it.
  //
  // A class of node:stream or one that extends it; the chunk types its
  // readable and writable sides have (null for a side it has not).
  bool isStreamClass(const ClassDecl *cls) const;
  Sema::StreamChunks streamChunksOf(const ClassDecl *cls) const;
  // The parameters of the method of node:stream that a method of the
  // program's own stream class overrides by its name, a hook the stream
  // calls (`_read`, `_write`, `_transform`...); none for another method.
  // The method may leave out their types, or the last of them, as in node
  // (see declareFunction); with others it is an error, as a variant of the
  // name the stream would never call (see linkOverrides).
  std::optional<std::vector<const Type *>> streamHookParams(const FunctionDecl *fn);
  // Whether the method has the name of a hook in a class of the program
  // that extends one of node:stream; before the classes are linked, whether
  // it may be one.
  bool isStreamHook(const FunctionDecl *fn, bool linked) const;
  void reportStreamHook(const FunctionDecl *fn, llvm::ArrayRef<FunctionDecl *> inherited);
  // Makes an instance of Duplex<W, R> a Writable<W> too.
  void addStreamBases(ClassDecl *inst, llvm::SMLoc loc);
  // The chunk type of an object-mode side at `site` without one: the
  // placeholder of a round that looks for it, what the program puts into it
  // in the last round, or null if nothing does. recordStreamChunk notes a
  // chunk of `value` put into a side of the placeholder `chunk` (false if it
  // is none).
  const Type *inferredChunkType(const std::string &site, llvm::SMLoc loc);
  bool recordStreamChunk(const Type *chunk, const Type *value, llvm::SMLoc loc);
  void noteStreamCallback(CallExpr *call, const VarSymbol *var);

private:
  Sema &sema_;
};

} // namespace tz
