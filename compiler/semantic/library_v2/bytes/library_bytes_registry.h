#pragma once

#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/program/library/bytes/buffer_method_table/buffer_method_table.h"
#include "compiler/program/library/bytes/bytes_method_table/bytes_method_table.h"
#include "compiler/program/library/bytes/number_method_table/number_method_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/library/bytes/bytes_check_helpers/bytes_check_helpers.h"
#include "compiler/semantic/library_v2/bytes/library_bytes_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <optional>
#include <string>
#include <vector>

namespace tz {

class LibraryBytesRegistry {
public:
  explicit LibraryBytesRegistry(SemanticState &owner);

  void bytesToString();
  void afterBufferEncodingArg();
  void finishView(const FinishView &task);
  void finishWrite(const FinishWrite &task);
  void nextWriteArg(const NextWriteArg &task);
  void afterByteFill(const AfterByteFill &task);
  void afterCopySort(const AfterCopySort &task);
  void afterWriteArg(const AfterWriteArg &task);
  void checkBytesNew(const CheckBytesNew &task);
  void finishTypedAt(const FinishTypedAt &task);
  void afterAllocFill(const AfterAllocFill &task);
  void afterAllocSize(const AfterAllocSize &task);
  void afterSetSource(const AfterSetSource &task);
  void afterWithIndex(const AfterWithIndex &task);
  void finishDataView(const FinishDataView &task);
  void finishFromWith(const FinishFromWith &task);
  void finishTypedSet(const FinishTypedSet &task);
  void nextBytesValue(const NextBytesValue &task);
  void nextValueSplit(const NextValueSplit &task);
  void walkBytesStore(const WalkBytesStore &task);
  void afterBytesValue(const AfterBytesValue &task);
  void afterConcatList(const AfterConcatList &task);
  void afterFixedValue(const AfterFixedValue &task);
  void afterSetLiteral(const AfterSetLiteral &task);
  void afterTypedValue(const AfterTypedValue &task);
  void afterValueSplit(const AfterValueSplit &task);
  void checkBytesIndex(const CheckBytesIndex &task);
  void finishAllocFill(const FinishAllocFill &task);
  void finishLengthNew(const FinishLengthNew &task);
  void afterBufferValue(const AfterBufferValue &task);
  void afterBytesSource(const AfterBytesSource &task);
  void afterConcatTotal(const AfterConcatTotal &task);
  void afterDecodeInput(const AfterDecodeInput &task);
  void afterIsBufferArg(const AfterIsBufferArg &task);
  void afterNonNegative(const AfterNonNegative &task);
  void checkBytesMember(const CheckBytesMember &task);
  void checkBytesMethod(const CheckBytesMethod &task);
  void checkBytesStatic(const CheckBytesStatic &task);
  void finishDecoderNew(const FinishDecoderNew &task);
  void writeEncodingArg(const WriteEncodingArg &task);
  void afterDecoderParse(const AfterDecoderParse &task);
  void afterSpreadValues(const AfterSpreadValues &task);
  void finishFatalDecode(const FinishFatalDecode &task);
  void finishFixedNumber(const FinishFixedNumber &task);
  void finishRangeMethod(const FinishRangeMethod &task);
  void finishTypedSearch(const FinishTypedSearch &task);
  void finishValueMethod(const FinishValueMethod &task);
  void afterByteLengthArg(const AfterByteLengthArg &task);
  void afterDataViewValue(const AfterDataViewValue &task);
  void continueDecoderNew(const ContinueDecoderNew &task);
  void finishDecoderParse(const FinishDecoderParse &task);
  void afterDataViewBuffer(const AfterDataViewBuffer &task);
  void afterDecoderOptions(const AfterDecoderOptions &task);
  void afterFromWithMapper(const AfterFromWithMapper &task);
  void afterFromWithSource(const AfterFromWithSource &task);
  void afterMappedCallback(const AfterMappedCallback &task);
  void afterTypedNewSource(const AfterTypedNewSource &task);
  void bufferEncodingValue(const BufferEncodingValue &task);
  void continueValueMethod(const ContinueValueMethod &task);
  void addByteCallbackFacts(const AddByteCallbackFacts &task);
  void afterDecoderLabelArg(const AfterDecoderLabelArg &task);
  void finishBufferToString(const FinishBufferToString &task);
  void finishVariableNumber(const FinishVariableNumber &task);
  void afterBufferFromSource(const AfterBufferFromSource &task);
  void afterTypedSearchValue(const AfterTypedSearchValue &task);
  void checkBytesUnionMember(const CheckBytesUnionMember &task);
  void checkBytesStaticMember(const CheckBytesStaticMember &task);
  void afterBytesIndexPosition(const AfterBytesIndexPosition &task);
  void checkByteCallbackResult(const CheckByteCallbackResult &task);

private:
  using ResultDecoderLabel = Result<const char *, LibraryBytesRegistryDecoderLabelError>;
  using ResultCountOfBytes = Result<int64_t, LibraryBytesRegistryCountOfBytesError>;
  using ResultOffsetAligned = Result<int64_t, LibraryBytesRegistryOffsetAlignedError>;
  using ResultBufferAligned = Result<int64_t, LibraryBytesRegistryBufferAlignedError>;
  using ResultSetSourceOk = Result<const Expr *, LibraryBytesRegistrySetSourceError>;
  using ResultIndexProven = Result<const Expr *, LibraryBytesRegistryWithIndexError>;
  using ResultVarValueFits = Result<const Expr *, LibraryBytesRegistryVarValueFitsError>;
  using ResultMapFitsType = Result<const Expr *, LibraryBytesRegistryTypedMapFitsError>;
  using ResultDecodedInput = Result<const Type *, LibraryBytesRegistryDecodedInputError>;
  using ResultViewedBuffer = Result<const Type *, LibraryBytesRegistryViewedBufferError>;
  using ResultBytesIndexing = Result<const Type *, LibraryBytesRegistryBytesIndexingError>;
  using ResultSourceElement = Result<const Type *, LibraryBytesRegistrySourceElementError>;
  using ResultConcatSources = Result<const Type *, LibraryBytesRegistryConcatSourcesError>;
  using ResultMapResultFits = Result<const Type *, LibraryBytesRegistryMapResultFitsError>;

  void pushLooseArgs(CallExpr *call);
  void pushFusedSource(CallExpr *call);
  void startAt(const TypedArrayDraft &typed);
  void startWith(const TypedArrayDraft &typed);
  void startWrite(const BufferCallDraft &buffer);
  void checkToJson(const BufferCallDraft &buffer);
  void pushPositions(CallExpr *call, size_t from);
  void reportSlice(const BufferCallDraft &buffer);
  void startEquals(const BufferCallDraft &buffer);
  void startTypedSet(const TypedArrayDraft &typed);
  void startToString(const BufferCallDraft &buffer);
  void startTypedFill(const TypedArrayDraft &typed);
  void continueTypedSet(const TypedArrayDraft &typed);
  void startConcat(CallExpr *call, MemberExpr *member);
  void startDecode(CallExpr *call, MemberExpr *member);
  void startEncode(CallExpr *call, MemberExpr *member);
  void startIsView(CallExpr *call, MemberExpr *member);
  void pushEncodingOrUtf8(CallExpr *call, size_t index);
  void startNumberMethod(const BufferCallDraft &buffer);
  void startTypedIterator(const TypedArrayDraft &typed);
  void startIsBuffer(CallExpr *call, MemberExpr *member);
  void continueDecode(CallExpr *call, MemberExpr *member);
  void startDataViewNew(CallExpr *call, const Type *type);
  void checkIsEncoding(CallExpr *call, MemberExpr *member);
  void checkSwap(const BufferCallDraft &buffer, int width);
  void reportBigIntArray(CallExpr *call, NameExpr *object);
  void startBufferFrom(CallExpr *call, MemberExpr *member);
  void startByteLength(CallExpr *call, MemberExpr *member);
  void startEncodeInto(CallExpr *call, MemberExpr *member);
  void startTextMethod(CallExpr *call, MemberExpr *member);
  void startTypedArrayOf(CallExpr *call, const Type *type);
  void startTypedArrayNew(CallExpr *call, const Type *type);
  void checkTextEncoderNew(CallExpr *call, const Type *type);
  void startBufferMethod(CallExpr *call, MemberExpr *member);
  void startBufferStatic(CallExpr *call, MemberExpr *member);
  void startTextDecoderNew(CallExpr *call, const Type *type);
  void startBufferCompare(CallExpr *call, MemberExpr *member);
  void startDataViewMethod(CallExpr *call, MemberExpr *member);
  void reportBytesNew(NewExpr *expr, const std::string &message);
  void startBufferFromString(CallExpr *call, MemberExpr *member);
  void startTextDecoderParse(CallExpr *call, MemberExpr *member);
  void startArrayBufferMethod(CallExpr *call, MemberExpr *member);
  void pushEncodingAt(const BufferCallDraft &buffer, size_t index);
  void walkNonNegative(const Expr *value, const std::string &what);
  void startAlloc(CallExpr *call, MemberExpr *member, bool isUnsafe);
  void reportEncodingWithoutText(const Expr *value, const Expr *encoding);
  void startArrayBufferNew(NewExpr *expr, CallExpr *call, const Type *type);
  void pushView(CallExpr *call, int64_t elementSize, const std::string &what);
  void startRangeMethod(const BufferCallDraft &buffer, BufferMethodKind kind);
  void toBuiltinCall(CallExpr *call, Builtin builtin, const Type *result) const;
  void walkSetRange(const Expr *target, const Expr *source, const Expr *offset);
  void rememberByteCallback(const Expr *arg, const Type *from, const Type *type);
  void startBytesValues(CallExpr *call, ArrayLitExpr *literal, const Type *type);
  void startJoin(const TypedArrayDraft &typed, const TypedArrayJoinMethod &join);
  void finishBytesValues(CallExpr *call, ArrayLitExpr *literal, const Type *type);
  void walkWriteLength(const Expr *object, const Expr *offset, const Expr *length);
  void reportStatic(CallExpr *call, MemberExpr *member, const std::string &message);
  void startBytesFromWith(CallExpr *call, const Type *type, const std::string &what);
  void reportUnprovenUpTo(const FlowProofs::ResultUpToWasProven &upTo, llvm::SMLoc at);
  void startTypedArrayMethod(CallExpr *call, MemberExpr *member, const Type *expected);
  void startFixedNumber(const BufferCallDraft &buffer, const BytesNumberMethod &number);
  void checkReverse(const TypedArrayDraft &typed, const TypedArrayReverseMethod &reverse);
  void startVariableNumber(const BufferCallDraft &buffer, const BytesNumberMethod &number);
  void startTypedSearch(const TypedArrayDraft &typed, const TypedArraySearchMethod &search);
  void reportMissingMethod(CallExpr *call, MemberExpr *member, const BytesMethodInfo *found);
  void reportUnprovenBetween(const FlowProofs::ResultBetweenProven &between, llvm::SMLoc at);
  void startCallbackCall(const TypedArrayDraft &typed, BytesMethod method, const Type *expected);
  void startTypedArrayFrom(CallExpr *call, MemberExpr *member, NameExpr *object, const Type *type);
  void startPositionsMethod(const TypedArrayDraft &typed,
                            const TypedArrayPositionsMethod &positions);
  void startSort(const TypedArrayDraft &typed, const TypedArraySortMethod &sort,
                 const Type *expected);
  void startValueMethod(const BufferCallDraft &buffer, BufferValueMethod method,
                        BytesSearchMode mode);
  void walkView(const Expr *buffer, const Expr *offset, const Expr *length, int64_t size,
                llvm::StringRef what);
  void finishBufferSearch(const BufferValueDraft &draft, Expr *value, Expr *encoding,
                          const std::vector<Expr *> &positions);


  bool canBeBuffer(const Type *type) const;
  bool isBufferType(const Type *type) const;
  bool isFatalFlag(const Expr *value) const;
  bool canBeFatal(const Expr *options) const;
  bool isBufferMethod(llvm::StringRef name) const;
  bool matchBytesSource(Expr *source, const Type *target);
  bool hasNewArity(CallExpr *call, unsigned min, unsigned max);
  bool matchDecoderLabels(const Expr *checked, llvm::SMLoc at);
  bool isSpreadElement(const ArrayLitExpr::Element &element) const;
  bool hasArity(CallExpr *call, unsigned min, unsigned max, llvm::SMLoc at);
  bool canNodeBeFatal(const Expr *options, std::vector<bool> &answers) const;
  bool hasUnionProperty(std::optional<BuiltinProperty> property, const Type *memberType) const;
  bool isSameView(const Expr *buffer, const Expr *offset, const Expr *length, int64_t size) const;


  const Type *uint8Type() const;
  const Type *bufferType() const;
  const Type *decoderOptionsType() const;
  const Type *valueElement(const Type *type) const;
  const Type *sourceElement(const Type *from) const;


  std::optional<std::u16string> literalText(const Expr *expr) const;
  std::optional<std::u16string> knownDecoderName(const Expr *label, const std::u16string &text);


  Expr *bytesText(Expr *expr);
  Expr *bytesMember(MemberExpr *member);
  Expr *reportNoMember(MemberExpr *member);
  Expr *bytesUnionMember(MemberExpr *member);
  Expr *bytesStaticMember(MemberExpr *member);
  Expr *keepObject(MemberExpr *member, Expr *value);
  Expr *makeBufferTest(CallExpr *call, MemberExpr *member);
  Expr *decoderNameLiteral(llvm::SMLoc loc, const std::u16string &name);
  Expr *checkViewMember(MemberExpr *member, std::optional<BuiltinProperty> property);
  Expr *checkDataViewMember(MemberExpr *member, std::optional<BuiltinProperty> property);
  Expr *checkArrayBufferMember(MemberExpr *member, std::optional<BuiltinProperty> property);
  Expr *checkTextDecoderMember(MemberExpr *member, std::optional<BuiltinProperty> property);
  Expr *checkTextEncoderMember(MemberExpr *member, std::optional<BuiltinProperty> property);
  Expr *readBuiltin(MemberExpr *member, Builtin builtin, const Type *result,
                    std::vector<Expr *> args = {});


  IntLitExpr *makeUtf8Literal(llvm::SMLoc loc) const;
  IntLitExpr *makeIntLiteral(llvm::SMLoc loc, int64_t value) const;


  CallExpr *makeTypedArrayFrom(const TypedArrayDraft &typed);
  CallExpr *makeArrayFrom(CallExpr *call, Expr *source, const Type *type);


  const Expr *withoutCasts(const Expr *expr) const;
  ResultSetSourceOk checkSetSource(const Expr *source) const;
  ResultDecodedInput checkDecodedInput(const Type *type) const;
  ResultBytesIndexing checkBytesIndexing(const Type *type) const;
  ResultViewedBuffer checkViewedBuffer(const Type *buffer) const;
  BoolLitExpr *makeBoolLiteral(llvm::SMLoc loc, bool value) const;
  ResultDecoderLabel checkDecoderLabel(llvm::StringRef label) const;
  NodeChildren<const Expr *> spreadOptions(const Expr *options) const;
  Signature mapperSignature(const Type *element, const Type *type) const;
  TypedArrayDraft describeCall(CallExpr *call, MemberExpr *member) const;
  ResultMapFitsType checkTypedMapFits(const TypedArrayDraft &typed) const;
  ResultMapResultFits checkMapResultFits(const Type *type, bool arrow) const;
  ResultIndexProven checkWithIndex(const Expr *array, const Expr *index) const;
  ResultConcatSources checkConcatSources(const Type *list, const Type *buffer) const;
  ResultSourceElement checkSourceElement(const Type *from, llvm::StringRef what) const;
  ResultVarValueFits checkVarValueFits(const Expr *value, int64_t width, bool isSigned);
  const VarSymbol *viewOwner(const Expr *expr, BuiltinProperty field, const Type **type) const;
  ResultCountOfBytes checkCountOfBytes(std::optional<int64_t> count, llvm::StringRef name) const;
  ResultBufferAligned checkBufferAligned(std::optional<int64_t> whole, int64_t size,
                                         llvm::StringRef what) const;
  ResultOffsetAligned checkOffsetAligned(std::optional<int64_t> start, int64_t size,
                                         llvm::StringRef what) const;

  SemanticState &state;
};

}
