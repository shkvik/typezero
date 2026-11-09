#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/fs/fs_checker/fs_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class FsErrors;
class CallChecker;
class ExprChecker;
class BytesChecker;
class TypeDeclarer;
class TypeRelations;
class ArgumentChecker;
class BuiltinModuleChecker;

class FsChecker {
public:
  FsChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkFsCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc);
  Expr *checkFsModuleCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc);
  Expr *checkTzIoCall(CallExpr *call, llvm::SMLoc loc);
  Expr *checkFsValue(llvm::StringRef name, Expr *expr);
  Expr *checkFsConstant(MemberExpr *member);
  bool nodeOptions(CallExpr *call, size_t index, llvm::StringRef fn,
                   llvm::ArrayRef<llvm::StringRef> allowed, llvm::StringMap<Expr *> &options);
  Expr *checkEncoding(Expr *encoding, llvm::StringRef fn, bool any);
  const Type *nativeError() const;
  void rememberNativeError();

private:
  using ResultOctalMode = Result<unsigned, FsCheckerOctalModeError>;

  static ResultOctalMode octalMode(llvm::StringRef text);
  static std::u16string utf16(llvm::StringRef text);

  SemanticState &state;
  FsErrors *fsErrors = nullptr;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  BytesChecker *bytesChecker = nullptr;
  TypeDeclarer *typeDeclarer = nullptr;
  TypeRelations *typeRelations = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  BuiltinModuleChecker *builtinModuleChecker = nullptr;
};

}
