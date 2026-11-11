#pragma once

#include "compiler/program/library/intrinsic/library_intrinsic_table/library_intrinsic_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/library/tz_modules/library_calls/library_calls_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class Scopes;
class ExprChecker;
class TypeResolver;
class StreamChecker;
class ArgumentChecker;
class WebCryptoChecker;

class LibraryCalls {
public:
  LibraryCalls(SemanticState &owner, SemanticParts &all);

  void prepareLibraries(SourceFile &program);
  bool inLibrary(llvm::SMLoc loc) const;
  Expr *checkLibCall(CallExpr *call, NameExpr *name);

private:
  using ResultIntrinsicShape = Result<const CallExpr *, LibraryCallsIntrinsicShapeError>;

  static ResultIntrinsicShape intrinsicShape(const CallExpr *call, const NameExpr *name,
                                             llvm::StringRef intrinsicName, bool known,
                                             unsigned args, unsigned typeArgs);

  struct IntrinsicCall {
    CallExpr *call;
    NameExpr *name;
    llvm::StringRef intrinsicName;
    const Type *typeArg;
  };

  Expr *checkIntrinsic(const IntrinsicCall &made, LibraryIntrinsic intrinsic);
  Expr *checkListenerIntrinsic(const IntrinsicCall &made, bool bound);
  static const Type *flatListenerType(const Type *typeArg);
  Expr *checkAsClass(const IntrinsicCall &made);
  Expr *checkAhead(const IntrinsicCall &made);
  static bool isAwaitable(const Type *type);
  Expr *checkWithContext(const IntrinsicCall &made);

  SemanticState &state;
  Scopes *scopes = nullptr;
  ExprChecker *exprChecker = nullptr;
  TypeResolver *typeResolver = nullptr;
  StreamChecker *streamChecker = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  WebCryptoChecker *webCryptoChecker = nullptr;
};

}
