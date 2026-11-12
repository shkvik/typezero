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
  bool isInLibrary(llvm::SMLoc loc) const;
  Expr *checkLibCall(CallExpr *call, NameExpr *name);

private:
  using ResultIntrinsicShape = Result<const CallExpr *, LibraryCallsIntrinsicShapeError>;

  ResultIntrinsicShape checkIntrinsicShape(const CallExpr *call, const NameExpr *name,
                                           llvm::StringRef intrinsicName, bool known, unsigned args,
                                           unsigned typeArgs) const;

  struct IntrinsicCall {
    CallExpr *call;
    NameExpr *name;
    llvm::StringRef intrinsicName;
    const Type *typeArg;
  };

  Expr *checkIntrinsic(const IntrinsicCall &made, LibraryIntrinsic intrinsic);
  enum class ListenerBinding {
    PLAIN,
    BOUND,
  };

  void prepareLibrary(Module *module);
  void rememberLibraryItem(Stmt *item, bool hasIntrinsics);
  void rememberLibraryFunction(FunctionDecl *functionDecl, bool hasIntrinsics);
  Expr *checkListenerIntrinsic(const IntrinsicCall &made, ListenerBinding binding);
  const Type *flatListenerType(const Type *typeArg) const;
  Expr *checkAsClass(const IntrinsicCall &made);
  Expr *checkAhead(const IntrinsicCall &made);
  bool isAwaitable(const Type *type) const;
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
