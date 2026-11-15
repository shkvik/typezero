#pragma once

#include "compiler/program/library/builtin_modules/node_option_table/node_option_table.h"
#include "compiler/program/library/fs/fs_class_shape/fs_class_shape.h"
#include "compiler/program/library/fs/fs_function_table/fs_function_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/library_v2/fs/library_fs_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <optional>
#include <string>

namespace tz {

class LibraryFsRegistry {
public:
  explicit LibraryFsRegistry(SemanticState &owner);

  void declareFsClasses();
  void afterFsModuleCall();
  void afterNativeError();
  void nativeErrorType();
  void rememberNativeError();
  void checkFsCall(const CheckFsCall &task);
  void finishRm(const FinishRm &task);
  void checkFsValue(const CheckFsValue &task);
  void finishMkdir(const FinishMkdir &task);
  void finishRmdir(const FinishRmdir &task);
  void afterPathArg(const AfterPathArg &task);
  void wrapFsResult(const WrapFsResult &task);
  void checkTzIoCall(const CheckTzIoCall &task);
  void finishReaddir(const FinishReaddir &task);
  void afterRmOptions(const AfterRmOptions &task);
  void afterWriteData(const AfterWriteData &task);
  void finishTwoPaths(const FinishTwoPaths &task);
  void finishReadline(const FinishReadline &task);
  void checkStatsDate(const CheckStatsDate &task);
  void checkFsConstant(const CheckFsConstant &task);
  void finishWriteFile(const FinishWriteFile &task);
  void afterEncodingArg(const AfterEncodingArg &task);
  void afterStatOptions(const AfterStatOptions &task);
  void afterMkdirOptions(const AfterMkdirOptions &task);
  void afterWriteOptions(const AfterWriteOptions &task);
  void checkReadlineCall(const CheckReadlineCall &task);
  void checkFsModuleCall(const CheckFsModuleCall &task);
  void checkFsClassMethod(const CheckFsClassMethod &task);
  void finishPathArgument(const FinishPathArgument &task);
  void afterReaddirOptions(const AfterReaddirOptions &task);
  void checkFsPromisesCall(const CheckFsPromisesCall &task);
  void afterFsPromisesCall(const AfterFsPromisesCall &task);
  void afterReadlineOptions(const AfterReadlineOptions &task);
  void afterPathCallOptions(const AfterPathCallOptions &task);
  void afterReadFileOptions(const AfterReadFileOptions &task);
  void readEncodingOrOptions(const ReadEncodingOrOptions &task);

private:
  enum class EncodingSet {
    ANY,
    UTF8_ONLY,
  };

  struct EncodingRead {
    bool isFailed;
    Expr *number;
  };

  struct FlagRead {
    bool isFailed;
    std::optional<bool> value;
  };

  using ResultOctalMode = Result<unsigned, LibraryFsRegistryOctalModeError>;
  using ResultInputIsStdin = Result<const Expr *, LibraryFsRegistryStdinInputError>;
  using ResultReadlineUsed = Result<llvm::StringRef, LibraryFsRegistryReadlineFunctionError>;
  using ResultPromiseFunction = Result<llvm::StringRef, LibraryFsRegistryPromiseFunctionError>;

  void startRm(const FsDraft &draft);
  void startStat(const FsDraft &draft);
  void startMkdir(const FsDraft &draft);
  void startRmdir(const FsDraft &draft);
  void startReaddir(const FsDraft &draft);
  void startReadFile(const FsDraft &draft);
  void startPathCall(const FsDraft &draft);
  void startTwoPaths(const FsDraft &draft);
  void startWriteFile(const FsDraft &draft);
  void startFsFunction(const FsDraft &draft);
  void startPathWithArgument(const FsDraft &draft);
  void pushDroppedCheck(Expr *value);
  void pushNonStreamChecks(CallExpr *call);
  void pushPathCheck(Expr **slot, FsPathForm form);
  void pushFsErrors(CallExpr *call, Builtin async);
  void pushValueCheck(Expr *value, const Type *type);


  bool literalTrue(const Expr *expr) const;
  bool isBytesData(const Type *type) const;
  bool isByteViewType(const Type *type) const;
  bool isFsConstants(const Expr *object) const;
  bool isFsModuleName(const NodeName *nodeName) const;
  bool hasFsArity(const FsDraft &draft, unsigned min, unsigned max);


  Builtin asyncFsBuiltin(Builtin sync) const;
  Builtin pathCallBuiltin(FsFunction function) const;
  Builtin twoPathsBuiltin(FsFunction function) const;
  Builtin pathArgumentBuiltin(FsFunction function) const;


  Expr *octalModeArgument(Expr *arg, llvm::SMLoc loc);
  Expr *pathArgumentValue(const FinishPathArgument &task);
  Expr *givenEncoding(const FsDraft &draft, size_t index) const;
  Expr *openFlag(const FsDraft &draft, Expr *flag, llvm::StringRef fallback);
  Expr *optionOf(const FsDraft &draft, size_t index, NodeOption option) const;


  const Type *fieldType(FsFieldType type) const;
  const Type *pathCallType(FsFunction function) const;


  llvm::StringRef fsErrorName(const CallExpr *call) const;
  llvm::StringRef plainFsErrorName(Builtin builtin) const;
  llvm::StringRef statErrorName(const CallExpr *call) const;
  llvm::StringRef writeErrorName(const CallExpr *call) const;


  std::string textOf(const Expr *expr) const;
  const Expr *argAt(const CallExpr *call, size_t index) const;
  std::optional<int> outputStream(const Expr *output);
  ClassDecl *declareFsClass(const FsClassInfo &shape);
  BoolLitExpr *makeBoolLiteral(llvm::SMLoc loc, bool value);
  IntLitExpr *makeIntLiteral(llvm::SMLoc loc, uint64_t value);
  StringLitExpr *makeStringLiteral(llvm::SMLoc loc, llvm::StringRef text);
  FlagRead readFlag(const FsDraft &draft, size_t index, NodeOption option);
  EncodingRead readEncoding(const FsDraft &draft, Expr *encoding, EncodingSet encodings);
  ResultOctalMode checkOctalMode(llvm::StringRef text) const;
  ResultInputIsStdin checkStdinInput(const Expr *input) const;
  ResultPromiseFunction checkPromiseFunction(llvm::StringRef functionName) const;
  ResultReadlineUsed checkReadlineFunction(llvm::StringRef functionName, NodeModule module) const;

  SemanticState &state;
};

}
