#pragma once

#include "compiler/program/library/builtin_names/library_function_table/library_function_table.h"
#include "compiler/program/library/stream/stream_function_table/stream_function_table.h"
#include "compiler/program/library/stream/stream_method_table/stream_method_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/library/stream/library_stream_registry_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallPtrSet.h"
#include <optional>
#include <string>
#include <vector>

namespace tz {

class LibraryStreamRegistry {
public:
  explicit LibraryStreamRegistry(SemanticState &owner);

  void noteTargetHandled();
  void afterEncodingValue();
  void nextEndArg(const NextEndArg &task);
  void afterEndArg(const AfterEndArg &task);
  void finishFsStream(const FinishFsStream &task);
  void finalFsStream(const FinalFsStream &task);
  void processStream(const ProcessStream &task);
  void afterHookError(const AfterHookError &task);
  void addStreamBases(const AddStreamBases &task);
  void afterStageMade(const AfterStageMade &task);
  void afterStageCall(const AfterStageCall &task);
  void startStageCall(const StartStageCall &task);
  void checkStreamNew(const CheckStreamNew &task);
  void finishFinished(const FinishFinished &task);
  void walkHookParams(const WalkHookParams &task);
  void afterFromAsync(const AfterFromAsync &task);
  void libraryAliasOf(const LibraryAliasOf &task);
  void afterByteChunk(const AfterByteChunk &task);
  void afterStageValue(const AfterStageValue &task);
  void afterStreamSide(const AfterStreamSide &task);
  void afterWriteChunk(const AfterWriteChunk &task);
  void checkStreamCall(const CheckStreamCall &task);
  void checkStreamFrom(const CheckStreamFrom &task);
  void afterFromSource(const AfterFromSource &task);
  void afterWriteSecond(const AfterWriteSecond &task);
  void finishStreamData(const FinishStreamData &task);
  void afterWrittenType(const AfterWrittenType &task);
  void afterFromOptions(const AfterFromOptions &task);
  void finishWritevHook(const FinishWritevHook &task);
  void nextComposedPart(const NextComposedPart &task);
  void afterEncodingType(const AfterEncodingType &task);
  void finishEncodedView(const FinishEncodedView &task);
  void afterComposedPart(const AfterComposedPart &task);
  void afterFromIterable(const AfterFromIterable &task);
  void finishDestroyHook(const FinishDestroyHook &task);
  void nextPipelineStage(const NextPipelineStage &task);
  void afterHookCallbacks(const AfterHookCallbacks &task);
  void afterPlainEncoding(const AfterPlainEncoding &task);
  void afterStageIterable(const AfterStageIterable &task);
  void afterFsStreamTypes(const AfterFsStreamTypes &task);
  void afterOverridesClass(const AfterOverridesClass &task);
  void afterStreamCallback(const AfterStreamCallback &task);
  void writtenCallbackType(const WrittenCallbackType &task);
  void afterFsStreamOption(const AfterFsStreamOption &task);
  void afterNativeIoFailure(const AfterNativeIoFailure &task);
  void checkStreamIntrinsic(const CheckStreamIntrinsic &task);
  void answerStreamCallback(const AnswerStreamCallback &task);
  void afterFinishedOptions(const AfterFinishedOptions &task);
  void afterFromOptionsType(const AfterFromOptionsType &task);
  void findStreamHookParams(const FindStreamHookParams &task);
  void finishPromisePipeline(const FinishPromisePipeline &task);
  void checkStreamLibraryCall(const CheckStreamLibraryCall &task);
  void finishCallbackPipeline(const FinishCallbackPipeline &task);
  void afterProcessStreamCall(const AfterProcessStreamCall &task);

private:
  struct StreamChunks {
    const Type *readable = nullptr;
    const Type *writable = nullptr;
  };

  struct StreamEscape {
    llvm::SMLoc loc;
    std::string how;
  };

  struct EncodingRun {
    MemberExpr *member;
    bool stdinStream;
    FieldPath path;
    const char *here;
    const char *start;
    std::vector<StreamEscape> escapes;
    llvm::SmallPtrSet<const Expr *, 16> safe;
  };

  struct NativeIoSignature {
    std::vector<const Type *> params;
    const Type *result;
  };

  struct WriteParts {
    Expr *encoding = nullptr;
    bool isEncodingChecked = false;
    Expr *callback = nullptr;
    bool isCallbackChecked = false;
  };

  using ResultStdinIsPlaced = Result<const Module *, LibraryStreamRegistryStdinPlaceError>;
  using ResultStreamSideOk = Result<const Type *, LibraryStreamRegistryStreamSideError>;
  using ResultWritableEnd = Result<const Type *, LibraryStreamRegistryWritableEndError>;
  using ResultReadableEnd = Result<const Type *, LibraryStreamRegistryReadableEndError>;
  using ResultDecodedChunks = Result<const Type *, LibraryStreamRegistryDecodedChunksError>;
  using ResultChunkIsGiven = Result<const Expr *, LibraryStreamRegistryChunkNotUndefinedError>;
  using ResultChunkTypeGiven = Result<const NewExpr *, LibraryStreamRegistryChunksGivenError>;
  using ResultOverrideArgs = Result<const ClassDecl *, LibraryStreamRegistryOverridesArgsError>;
  using ResultConcreteStream = Result<llvm::StringRef, LibraryStreamRegistryConcreteStreamError>;
  using ResultHookParamsMatch = Result<const FunctionDecl *, LibraryStreamRegistryHookParamsError>;
  using ResultMethodExists =
      Result<const FunctionDecl *, LibraryStreamRegistryOverriddenMethodError>;

  void collectEscapes(EncodingRun &run);
  void startPipeline(const StreamFunctionDraft &stream);
  void startFinished(const StreamFunctionDraft &stream);
  void startCompose(const StreamFunctionDraft &stream);
  void finishCompose(const ComposeDraft &draft);
  void finishPipeline(const PipelineDraft &draft);
  void failPipelineStage(const PipelineDraft &draft);
  void startStreamEnd(const StreamDataDraft &stream);
  void startStreamRead(const StreamDataDraft &stream);
  void startStreamWrite(const StreamDataDraft &stream);
  void pushArgValue(Expr *arg, const Type *type);
  void markEscapeWindow(EncodingRun &run) const;
  void reportPipe(CallExpr *call, MemberExpr *member);
  void pushEncodingValue(Expr *arg, bool isChecked);
  void pushLooseChecks(CallExpr *call, size_t from);
  void finishFromIterator(CallExpr *call, llvm::SMLoc loc);
  void reportFirstEscape(const EncodingRun &run, llvm::SMLoc loc);
  void checkNativeIo(CallExpr *call, const Type *failure);
  void pushUndefinedValue(llvm::SMLoc loc, const Type *type);
  void failStreamNew(NewExpr *expr, const std::string &message);
  void startOverridesIntrinsic(CallExpr *call, NameExpr *name);
  void pushCheckedAs(Expr *arg, bool isChecked, const Type *type);
  void checkChunkIntrinsic(CallExpr *call, const Type *typeArg);
  void classifyUse(EncodingRun &run, const Expr *expr) const;
  void pushEncodedStreamType(ClassDecl *classDecl, llvm::SMLoc loc);
  void continueStreamEnd(const StreamDataDraft &stream, unsigned checked);
  void failEndCallback(const StreamDataDraft &stream, const Expr *arg);
  void continueFinished(const StreamFunctionDraft &stream, const Type *options);
  void startSetEncoding(CallExpr *call, MemberExpr *member, ClassDecl *classDecl);
  void scanOptionsLiteral(const ObjectLitExpr *literal, FsStreamDraft &args) const;
  void addStreamChunk(StreamChunks &chunks, const ClassDecl *classDecl) const;
  void continueStreamWrite(const StreamDataDraft &stream, const WriteParts &parts);
  void failStageKind(const PipelineDraft &draft, const StageShapeDraft &shape, Expr *arg);
  void functionStage(const PipelineDraft &draft, const StageShapeDraft &shape, Expr *arg);
  void startFsStreamFunction(CallExpr *call, FunctionDecl *functionDecl, llvm::SMLoc loc);
  void iterableStage(const PipelineDraft &draft, const StageShapeDraft &shape, Expr *iterator);
  void sourceFunctionStage(const PipelineDraft &draft, const StageShapeDraft &shape, Expr *arg);
  void iterableSourceStage(const PipelineDraft &draft, const StageShapeDraft &shape, Expr *arg);
  void rememberHook(const FunctionDecl *functionDecl, bool isHooked,
                    std::vector<const Type *> params);
  void pushFromFunction(CallExpr *call, LibraryFunction function, const Type *chunk,
                        llvm::SMLoc loc);
  void startStreamData(CallExpr *call, MemberExpr *member, ClassDecl *classDecl,
                       StreamMethod method);
  void startWritevHook(const FunctionDecl *functionDecl, const StreamSidesDraft &sides,
                       const Type *callback);
  void pushChunkValue(const StreamDataDraft &stream, Expr *arg, StreamChunkSide side,
                      bool isChecked);
  void pushPlainSetEncoding(CallExpr *call, MemberExpr *member, ClassDecl *classDecl,
                            const Type *receiver);
  void sinkFunctionStage(const PipelineDraft &draft, const StageShapeDraft &shape, Expr *arg,
                         const Type *functionType);
  void streamClassStage(PipelineDraft draft, const StageShapeDraft &shape, Expr *arg,
                        ClassDecl *classDecl);
  void pushStageCall(const StageShapeDraft &shape, LibraryFunction function,
                     std::vector<const Type *> bound, Expr *value, const Type *coerceTo);
  void noteEscape(EncodingRun &run, const Expr *expr,
                  const llvm::SmallPtrSetImpl<const Expr *> &direct) const;


  bool isByteChunk(const Type *type) const;
  bool isNativeIo(Builtin builtin) const;
  bool isStdinCall(const Expr *expr) const;
  bool isUntypedParam(const Type *type) const;
  bool isListeningMethod(llvm::StringRef name) const;
  bool isStreamClass(const ClassDecl *classDecl) const;
  bool isStreamHook(const FunctionDecl *functionDecl) const;
  bool isStreamTaking(const Type *writable, const Type *chunk);
  bool isChunkValue(bool isOnlyStrings, const Type *candidate) const;
  bool isTypedLibraryStream(const ClassDecl *classDecl) const;
  bool isBefore(const EncodingRun &run, const Expr *expr) const;
  bool isOwnBlock(const VarSymbol *root, bool stdinStream) const;
  bool isEncodingPath(const EncodingRun &run, const Expr *expr) const;
  bool isInModule(const std::string &place, llvm::StringRef module) const;
  bool hasStreamArity(const StreamDataDraft &stream, unsigned min, unsigned max);
  bool reportWritableEnd(const PipelineDraft &draft, const StageShapeDraft &shape,
                         const StreamChunks &chunks, const Type *type);


  const Type *optionalString() const;
  const Type *pipelineStageType(const StageShapeDraft &shape) const;


  unsigned chunkKind(const Type *typeArg) const;
  VarSymbol *processStdinVar();
  Expr *valuesIteratorOf(Expr *collection);
  const Expr *withoutImplicit(const Expr *expr) const;
  std::vector<Expr *> takeCheckedValues(size_t count);
  ResultStdinIsPlaced checkStdinPlace() const;
  const ClassDecl *libraryBase(const ClassDecl *classDecl) const;
  StreamChunks streamChunksOf(const ClassDecl *classDecl) const;
  std::vector<const Type *> encodedArgs(const ClassDecl *classDecl) const;
  StringLitExpr *typeNameLiteral(CallExpr *call, const Type *typeArg);
  std::optional<FieldPath> encodingPath(const Expr *object, bool stdinStream);
  ArrayLitExpr *stageList(const PipelineDraft &draft, Expr *&lastStream);
  std::vector<const Stmt *> encodingBody(const EncodingRun &run) const;
  std::optional<llvm::StringRef> writableField(const FieldPath &path) const;
  const FuncSymbol *libraryHook(llvm::ArrayRef<FunctionDecl *> inherited) const;
  NativeIoSignature nativeIoSignature(Builtin builtin, const Type *failure) const;
  ResultWritableEnd checkWritableEnd(const StreamChunks &chunks, const Type *type) const;
  ResultReadableEnd checkReadableEnd(const StreamChunks &chunks, const Type *type) const;
  ResultDecodedChunks checkDecodedChunks(const Type *readable, const Type *receiver) const;
  std::optional<StreamSidesDraft> streamSides(const ClassDecl *lib, const Type *streamError) const;
  std::optional<std::string> encodingPathProblem(const std::optional<FieldPath> &path,
                                                 bool stdinStream) const;
  ResultOverrideArgs checkOverridesArgs(const ClassDecl *classDecl,
                                        const StringLitExpr *literal) const;
  ResultConcreteStream checkConcreteStream(llvm::StringRef name, bool readable,
                                           bool writable) const;
  ResultChunkTypeGiven checkChunksGiven(const NewExpr *expr, const ClassDecl *generic,
                                        const Type *expected) const;
  ResultHookParamsMatch checkHookParams(const FunctionDecl *functionDecl,
                                        llvm::ArrayRef<FunctionDecl *> inherited) const;
  ResultChunkIsGiven checkChunkNotUndefined(const Expr *arg, StreamChunkSide side,
                                            llvm::StringRef method) const;
  ResultStreamSideOk checkStreamSide(const Type *chunk, const Type *receiver, llvm::StringRef name,
                                     bool reading) const;
  ResultMethodExists checkOverriddenMethod(const ClassDecl *classDecl,
                                           llvm::ArrayRef<FunctionDecl *> methods,
                                           llvm::StringRef method) const;

  SemanticState &state;
};

}
