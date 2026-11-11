#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/stream/set_encoding_checker/set_encoding_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "llvm/ADT/SmallPtrSet.h"

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

  struct StreamEscape {
    llvm::SMLoc loc;
    std::string how;
  };

  struct EncodingRun {
    MemberExpr *member = nullptr;
    bool stdinStream = false;
    FieldPath path;
    const char *here = nullptr;
    const char *start = nullptr;
    std::vector<StreamEscape> escapes;
    llvm::SmallPtrSet<const Expr *, 16> safe;
  };

  Expr *plainSetEncoding(CallExpr *call, MemberExpr *member, ClassDecl *cls, const Type *receiver);
  static const Expr *withoutImplicit(const Expr *expr);
  static bool isStdinCall(const Expr *expr);
  std::optional<FieldPath> encodingPath(const Expr *object, bool stdinStream);
  std::optional<std::string> encodingPathProblem(const std::optional<FieldPath> &path,
                                                 bool stdinStream) const;
  static std::optional<llvm::StringRef> writableField(const FieldPath &path);
  bool isOwnBlock(const VarSymbol *root, bool stdinStream) const;
  static void markEscapeWindow(EncodingRun &run);
  static bool isBefore(const EncodingRun &run, const Expr *e);
  bool isEncodingPath(const EncodingRun &run, const Expr *e) const;
  void collectEscapes(EncodingRun &run);
  std::vector<const Stmt *> encodingBody(const EncodingRun &run) const;
  void classifyUse(EncodingRun &run, const Expr *e) const;
  void noteEscape(EncodingRun &run, const Expr *e,
                  const llvm::SmallPtrSetImpl<const Expr *> &direct) const;
  void reportFirstEscape(const EncodingRun &run, llvm::SMLoc loc);

  SemanticState &state;
  ChunkTypes *chunkTypes = nullptr;
  FieldFacts *fieldFacts = nullptr;
  StreamSources *streamSources = nullptr;
  EmitterChecker *emitterChecker = nullptr;
  ProcessChecker *processChecker = nullptr;
  GenericClassInstantiator *genericClassInstantiator = nullptr;
};

}
