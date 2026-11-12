#include "compiler/semantic/library/stream/stream_new_checker/stream_new_checker.h"
#include "compiler/program/library/builtin_names/library_class_table/library_class_table.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/declare/class_declarer/class_declarer.h"
#include "compiler/semantic/library/tz_modules/library_calls/library_calls.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"

namespace tz {

StreamNewChecker::StreamNewChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->exprChecker = &all.exprChecker;
  this->libraryCalls = &all.libraryCalls;
  this->classDeclarer = &all.classDeclarer;
}

StreamNewChecker::ResultConcreteStream
StreamNewChecker::checkConcreteStream(llvm::StringRef name, bool readable, bool writable) const {
  if (!LibraryClassTable::isName(name, LibraryClass::PASS_THROUGH)) {
    auto message = ErrorTextStreamNew::abstract(name, readable, writable);

    return err(StreamNewCheckerConcreteStreamError{
        .code = StreamNewCheckerConcreteStreamErrorCode::ABSTRACT_STREAM,
        .message = message,
    });
  }

  return ok(name);
}

StreamNewChecker::ResultChunkTypeGiven
StreamNewChecker::checkChunksGiven(const NewExpr *expr, const ClassDecl *generic,
                                   const Type *expected) const {
  const Type *want = expected != nullptr ? expected->nonOptional() : nullptr;
  const ClassDecl *wanted = want != nullptr ? this->classDeclarer->classOf(want) : nullptr;
  bool isFromExpected = wanted != nullptr && wanted->generic() == generic;
  if (expr->typeArgs.empty() && !isFromExpected) {
    auto message = ErrorTextStreamNew::untyped(generic->name);

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
  bool isReadable = LibraryClassTable::isName(name, LibraryClass::READABLE);
  bool isWritable = LibraryClassTable::isName(name, LibraryClass::WRITABLE);
  bool isDuplex = LibraryClassTable::isName(name, LibraryClass::DUPLEX) ||
                  LibraryClassTable::isName(name, LibraryClass::TRANSFORM) ||
                  LibraryClassTable::isName(name, LibraryClass::PASS_THROUGH);
  if (!isReadable && !isWritable && !isDuplex) {
    return nullptr;
  }
  if (this->libraryCalls->isInLibrary(expr->loc())) {
    return nullptr;
  }
  auto concrete = this->checkConcreteStream(name, isReadable, isWritable);
  if (concrete.isErr()) {
    return this->failedNew(expr, concrete.error().message);
  }
  auto given = this->checkChunksGiven(expr, generic, expected);
  if (given.isErr()) {
    return this->failedNew(expr, given.error().message);
  }

  return nullptr;
}

}
