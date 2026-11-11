#include "compiler/semantic/library/stream/stream_new_checker/stream_new_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/library/tz_modules/library_calls/library_calls.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"

namespace tz {

StreamNewChecker::StreamNewChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->exprChecker = &all.exprChecker;
  this->libraryCalls = &all.libraryCalls;
}

StreamNewChecker::ResultConcreteStream
StreamNewChecker::concreteStream(llvm::StringRef name, bool readable, bool writable) {
  if (name != "PassThrough") {
    std::string message = ErrorTextStreamNew::abstract(name, readable, writable);

    return err(StreamNewCheckerConcreteStreamError{
        .code = StreamNewCheckerConcreteStreamErrorCode::ABSTRACT_STREAM,
        .message = message,
    });
  }

  return ok(name);
}

StreamNewChecker::ResultChunkTypeGiven StreamNewChecker::chunksGiven(const NewExpr *expr,
                                                                     const ClassDecl *generic,
                                                                     const Type *expected) {
  const Type *want = expected != nullptr ? expected->nonOptional() : nullptr;
  const ClassDecl *wanted = want != nullptr ? ClassDeclarer::classOf(want) : nullptr;
  bool isFromExpected = wanted != nullptr && wanted->generic() == generic;
  if (expr->typeArgs.empty() && !isFromExpected) {
    std::string message = ErrorTextStreamNew::untyped(generic->name);

    return err(StreamNewCheckerChunksGivenError{
        .code = StreamNewCheckerChunksGivenErrorCode::CHUNK_TYPE_MISSING,
        .message = message,
    });
  }

  return ok(expr);
}

Expr *StreamNewChecker::failedNew(NewExpr *expr, const std::string &message) {
  this->state.getDiagnostics().error(expr->loc(), message);
  expr->type = Type::error();
  for (Expr *&arg : expr->args) {
    arg = this->exprChecker->checkExpr(arg);
  }

  return expr;
}

Expr *StreamNewChecker::checkStreamNew(NewExpr *expr, ClassDecl *generic, const Type *expected) {
  llvm::StringRef name = generic->name;
  bool isReadable = name == "Readable";
  bool isWritable = name == "Writable";
  bool isDuplex = name == "Duplex" || name == "Transform" || name == "PassThrough";
  if (!isReadable && !isWritable && !isDuplex) {
    return nullptr;
  }
  if (this->libraryCalls->inLibrary(expr->loc())) {
    return nullptr;
  }
  ResultConcreteStream concrete = StreamNewChecker::concreteStream(name, isReadable, isWritable);
  if (concrete.isErr()) {
    return this->failedNew(expr, concrete.error().message);
  }
  ResultChunkTypeGiven given = StreamNewChecker::chunksGiven(expr, generic, expected);
  if (given.isErr()) {
    return this->failedNew(expr, given.error().message);
  }

  return nullptr;
}

}
