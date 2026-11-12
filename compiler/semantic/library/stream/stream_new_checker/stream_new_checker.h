#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/stream/stream_new_checker/stream_new_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class ExprChecker;
class LibraryCalls;
class ClassDeclarer;

class StreamNewChecker {
public:
  StreamNewChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkStreamNew(NewExpr *expr, ClassDecl *generic, const Type *expected);

private:
  using ResultConcreteStream = Result<llvm::StringRef, StreamNewCheckerConcreteStreamError>;
  using ResultChunkTypeGiven = Result<const NewExpr *, StreamNewCheckerChunksGivenError>;

  ResultConcreteStream checkConcreteStream(llvm::StringRef name, bool readable,
                                           bool writable) const;
  ResultChunkTypeGiven checkChunksGiven(const NewExpr *expr, const ClassDecl *generic,
                                        const Type *expected) const;

  Expr *failedNew(NewExpr *expr, const std::string &message);

  SemanticState &state;
  ExprChecker *exprChecker = nullptr;
  LibraryCalls *libraryCalls = nullptr;
  ClassDeclarer *classDeclarer = nullptr;
};

}
