#pragma once

#include "compiler/program/library/fs/fs_function_table/fs_function_table.h"
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

  Expr *checkFsCall(CallExpr *call, llvm::StringRef functionName, llvm::SMLoc loc);
  Expr *checkFsModuleCall(CallExpr *call, llvm::StringRef functionName, llvm::SMLoc loc);
  Expr *checkTzIoCall(CallExpr *call, llvm::SMLoc loc);
  Expr *checkFsValue(llvm::StringRef name, Expr *expr);
  Expr *checkFsConstant(MemberExpr *member);
  bool nodeOptions(CallExpr *call, size_t index, llvm::StringRef functionName,
                   llvm::ArrayRef<llvm::StringRef> allowed, llvm::StringMap<Expr *> &options);
  Expr *checkEncoding(Expr *encoding, llvm::StringRef functionName, bool any);
  const Type *nativeError() const;
  void rememberNativeError();

private:
  using ResultOctalMode = Result<unsigned, FsCheckerOctalModeError>;

  struct FsCall {
    CallExpr *call = nullptr;
    FsFunction function = FsFunction::READ_FILE_SYNC;
    llvm::StringRef fn;
    llvm::SMLoc loc;
    llvm::StringMap<Expr *> options;
  };

  ResultOctalMode octalMode(llvm::StringRef text) const;

  Expr *checkLoosely(CallExpr *call);
  Expr *checkPath(Expr *arg, bool fd);
  BoolLitExpr *makeBoolLiteral(llvm::SMLoc loc, bool value);
  IntLitExpr *makeIntLiteral(llvm::SMLoc loc, uint64_t value);
  StringLitExpr *makeStringLiteral(llvm::SMLoc loc, llvm::StringRef text);
  Expr *openFlag(FsCall &fs, Expr *flag, llvm::StringRef fallback);
  Expr *reportNotFunction(CallExpr *call, llvm::StringRef functionName, llvm::SMLoc loc);
  Expr *reportUnknownFsFunction(CallExpr *call, llvm::StringRef functionName, llvm::SMLoc loc);
  bool literalFlag(FsCall &fs, Expr *value, llvm::StringRef option, bool &out);
  bool encodingOrOptions(FsCall &fs, size_t index, llvm::ArrayRef<llvm::StringRef> allowed);
  Expr *checkReadFile(FsCall &fs);
  Expr *checkWriteFile(FsCall &fs);
  Expr *checkPathCall(FsCall &fs);
  Expr *checkPathWithArgument(FsCall &fs);
  Expr *checkStat(FsCall &fs);
  Expr *checkReaddir(FsCall &fs);
  Expr *checkMkdir(FsCall &fs);
  Expr *checkRmdir(FsCall &fs);
  Expr *checkRm(FsCall &fs);
  Expr *checkTwoPaths(FsCall &fs);

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
