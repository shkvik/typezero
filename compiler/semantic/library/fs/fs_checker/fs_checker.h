#pragma once

#include "compiler/program/library/builtin_modules/node_option_table/node_option_table.h"
#include "compiler/program/library/fs/fs_function_table/fs_function_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/library/fs/fs_checker/fs_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "llvm/ADT/DenseMap.h"

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
struct NodeName;

using NodeOptions = llvm::SmallDenseMap<NodeOption, Expr *>;

class FsChecker {
public:
  FsChecker(SemanticState &owner, SemanticParts &all);

  Expr *checkFsCall(CallExpr *call, llvm::StringRef functionName, llvm::SMLoc loc);
  Expr *checkFsModuleCall(CallExpr *call, llvm::StringRef functionName, llvm::SMLoc loc);
  Expr *checkTzIoCall(CallExpr *call, llvm::SMLoc loc);
  Expr *checkFsValue(llvm::StringRef name, Expr *expr);
  Expr *checkFsConstant(MemberExpr *member);
  enum class EncodingSet {
    ANY,
    UTF8_ONLY,
  };

  bool readNodeOptions(CallExpr *call, size_t index, llvm::StringRef functionName,
                       llvm::ArrayRef<NodeOption> allowed, NodeOptions &options);
  Expr *checkEncoding(Expr *encoding, llvm::StringRef functionName, EncodingSet encodings);
  const Type *nativeError() const;
  void rememberNativeError();

private:
  using ResultOctalMode = Result<unsigned, FsCheckerOctalModeError>;

  enum class PathForm {
    PATH_OR_DESCRIPTOR,
    PATH_ONLY,
  };

  struct FsCall {
    CallExpr *call = nullptr;
    FsFunction function = FsFunction::READ_FILE_SYNC;
    llvm::StringRef functionName;
    llvm::SMLoc loc;
    NodeOptions options = NodeOptions();
  };

  struct FlagRead {
    bool isFailed;
    std::optional<bool> value;
  };

  ResultOctalMode checkOctalMode(llvm::StringRef text) const;
  Expr *checkPath(Expr *arg, PathForm form);
  BoolLitExpr *makeBoolLiteral(llvm::SMLoc loc, bool value);
  IntLitExpr *makeIntLiteral(llvm::SMLoc loc, uint64_t value);
  StringLitExpr *makeStringLiteral(llvm::SMLoc loc, llvm::StringRef text);
  Expr *openFlag(FsCall &fs, Expr *flag, llvm::StringRef fallback);
  Expr *reportNotFunction(CallExpr *call, llvm::StringRef functionName, llvm::SMLoc loc);
  Expr *reportUnknownFsFunction(CallExpr *call, llvm::StringRef functionName, llvm::SMLoc loc);
  FlagRead readFlag(FsCall &fs, NodeOption option);
  bool readEncodingOrOptions(FsCall &fs, size_t index, llvm::ArrayRef<NodeOption> allowed);
  std::vector<llvm::StringRef> optionNames(llvm::ArrayRef<NodeOption> options) const;
  Expr *givenEncoding(FsCall &fs) const;
  Expr *boolOption(FsCall &fs, NodeOption option);
  Expr *checkReadFile(FsCall &fs);
  Expr *checkWriteFile(FsCall &fs);
  bool isBytesData(const Type *type) const;
  bool isByteViewType(const Type *type) const;
  Expr *checkPathCall(FsCall &fs);
  Builtin pathCallBuiltin(FsFunction function) const;
  const Type *pathCallType(FsFunction function) const;
  Expr *checkPathWithArgument(FsCall &fs);
  Expr *pathArgument(FsCall &fs);
  Expr *octalModeArgument(Expr *arg, llvm::SMLoc loc);
  Builtin pathArgumentBuiltin(FsFunction function) const;
  Expr *checkStat(FsCall &fs);
  Expr *checkReaddir(FsCall &fs);
  Expr *checkMkdir(FsCall &fs);
  Expr *checkRmdir(FsCall &fs);
  Expr *checkRm(FsCall &fs);
  Expr *checkTwoPaths(FsCall &fs);
  Builtin twoPathsBuiltin(FsFunction function) const;
  bool isFsConstants(const Expr *object) const;
  bool isFsModuleName(const NodeName *nodeName) const;

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
