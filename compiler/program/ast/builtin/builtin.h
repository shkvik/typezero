#pragma once

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/SMLoc.h"
#include <cstdint>
#include <deque>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tz {

enum class Builtin {
  None,
  ConsoleLog,
  ConsoleError,
  ProcessArgv,
  ProcessEnv,
  ProcessExit,
  ProcessExitCode,
  ProcessSetExitCode,
  ProcessCwd,
  ProcessExecPath,
  ProcessPlatform,
  ProcessStdoutWrite,
  ProcessStderrWrite,
  ProcessIsTTY,
  DateNow,
  PerformanceNow,
  StringOf,
  NumberOf,
  ParseInt,
  ParseFloat,
  IsInteger,
  IsSafeInteger,
  FromCharCode,
  FromCodePoint,
  UriEncodeComponent,
  UriEncode,
  UriDecodeComponent,
  UriDecode,
  NumToFixed,
  NumToRadix,
  NumConvert,
  StrCharCodeAt,
  StrCharAt,
  StrCodePointAt,
  StrAt,
  StrSlice,
  StrSubstring,
  StrIndexOf,
  StrLastIndexOf,
  StrIncludes,
  StrStartsWith,
  StrEndsWith,
  StrRepeat,
  StrToString,
  StrConcat,
  StrSplit,
  StrTrim,
  StrTrimStart,
  StrTrimEnd,
  StrPadStart,
  StrPadEnd,
  StrToUpperCase,
  StrToLowerCase,
  StrReplace,
  StrReplaceAll,
  ArrPush,
  ArrPop,
  ArrShift,
  ArrUnshift,
  ArrAt,
  ArrSlice,
  ArrConcat,
  ArrIndexOf,
  ArrLastIndexOf,
  ArrIncludes,
  ArrJoin,
  ArrReverse,
  ArrFill,
  ArrSort,
  ArrToString,
  ArrSplice,
  ArrForEach,
  ArrMap,
  ArrFilter,
  ArrFlatMap,
  ArrReduce,
  ArrReduceRight,
  ArrFind,
  ArrFindIndex,
  ArrFindLast,
  ArrFindLastIndex,
  ArrSome,
  ArrEvery,
  ArrSortWith,
  ObjectKeys,
  ObjectValues,
  ObjectEntries,
  ArrKeys,
  ArrValues,
  ArrEntries,
  IterFrom,
  ArrayFrom,
  IterNext,
  IterMap,
  IterFilter,
  IterTake,
  IterDrop,
  IterFlatMap,
  IterToArray,
  IterForEach,
  IterReduce,
  IterSome,
  IterEvery,
  IterFind,
  MathFloor,
  MathCeil,
  MathRound,
  MathTrunc,
  MathAbs,
  MathSign,
  MathMin,
  MathMax,
  MathPow,
  MathSqrt,
  MathCbrt,
  MathExp,
  MathExpm1,
  MathLog,
  MathLog1p,
  MathLog2,
  MathLog10,
  MathSin,
  MathCos,
  MathTan,
  MathAsin,
  MathAcos,
  MathAtan,
  MathAtan2,
  MathSinh,
  MathCosh,
  MathTanh,
  MathAsinh,
  MathAcosh,
  MathAtanh,
  MathHypot,
  MathFround,
  MathClz32,
  MathImul,
  MathClamp,
  BitRotateLeft,
  BitRotateRight,
  BitLeadingZeros,
  BitTrailingZeros,
  BitOnesCount,
  BitReverseBytes,
  BitMul64,
  BitAdd64,
  BitSub64,
  BitWrapFrom,
  MathRandom,
  DecimalOf,
  DecMax,
  DecMin,
  DecSum,
  DecDiv,
  DecRound,
  DecToFixed,
  DecToFormat,
  DecToString,
  DecToExponential,
  DecToPrecision,
  DecSqrt,
  DecAbs,
  DecIsInteger,
  DecDecimalPlaces,
  DecShiftedBy,
  DecToFraction,
  ResultOk,
  ResultErr,
  ResultCombine,
  ResultCombineWithAllErrors,
  ResultIsOk,
  ResultIsErr,
  ResultMap,
  ResultMapErr,
  ResultAndThen,
  ResultOrElse,
  ResultMatch,
  ResultUnwrapOr,
  ResultAndTee,
  ResultOrTee,
  ResultAndThrough,
  RegExpNew,
  RegExpParse,
  RegExpEscape,
  RegExpTest,
  RegExpExec,
  RegExpToString,
  RegExpSource,
  RegExpFlags,
  RegExpFlag,
  StrMatch,
  StrMatchGlobal,
  StrMatchAll,
  StrSearch,
  StrReplaceRegex,
  StrReplaceRegexFn,
  StrSplitRegex,
  JsonParse,
  JsonParseAs,
  FromJson,
  JsonStringify,
  EmbedJson,
  ArrayIsArray,
  WeakRefNew,
  WeakRefDeref,
  DateNew,
  DateCopy,
  DateFromString,
  DateFromFields,
  DateUTC,
  DateParse,
  DateText,
  DateGetTime,
  DateGetUTC,
  DateGetLocal,
  DateSetTime,
  DateSetUTC,
  DateSetLocal,
  DateToISOString,
  DateToJSON,
  DateToUTCString,
  DateToLocalString,
  DateToLocale,
  DtfNew,
  DtfFormat,
  DtfFormatToParts,
  DtfResolvedOptions,
  IntlNew,
  IntlSupportedLocales,
  IntlCanonicalLocales,
  IntlResolvedOptions,
  IntlBound,
  NumToLocale,
  NfFormat,
  NfFormatToParts,
  CollatorCompare,
  StrLocaleCompare,
  StrNormalize,
  PrSelect,
  RtfFormat,
  RtfFormatToParts,
  LfFormat,
  LfFormatToParts,
  DnOf,
  SegSegment,
  SegmentsArray,
  SegmentsContaining,
  PathJoin,
  PathResolve,
  PathNormalize,
  PathIsAbsolute,
  PathDirname,
  PathBasename,
  PathExtname,
  PathRelative,
  PathParse,
  PathFormat,
  PathToNamespacedPath,
  OsString,
  OsNumber,
  OsAvailableParallelism,
  OsCpus,
  OsLoadavg,
  OsUserInfo,
  ReadLine,
  FsReadFile,
  FsWriteFile,
  FsExists,
  FsAccess,
  FsChmod,
  FsTruncate,
  FsStat,
  FsReaddir,
  FsMkdir,
  FsMkdtemp,
  FsRmdir,
  FsUnlink,
  FsRm,
  FsRename,
  FsCopyFile,
  FsRealpath,
  FsIsKind,
  FsStatsDate,
  FspReadFile,
  FspWriteFile,
  FspAccess,
  FspChmod,
  FspTruncate,
  FspStat,
  FspReaddir,
  FspMkdir,
  FspMkdtemp,
  FspRmdir,
  FspUnlink,
  FspRm,
  FspRename,
  FspCopyFile,
  FspRealpath,
  FspOpen,
  FhFd,
  FhClose,
  FhReadFile,
  FhWriteFile,
  FhWrite,
  FhStat,
  FhTruncate,
  FhSync,
  RlCreate,
  RlQuestion,
  RlQuestionCallback,
  RlClose,
  ChildSpawnSync,
  ChildExecFileSync,
  ChildExecSync,
  MapGet,
  MapSet,
  MapHas,
  MapDelete,
  MapClear,
  MapSize,
  SetAdd,
  RecordSet,
  RecordNullishSet,
  MapForEach,
  MapKeys,
  MapValues,
  MapEntries,
  SetValues,
  SetEntries,
  SetUnion,
  SetIntersection,
  SetDifference,
  SetSymmetricDifference,
  SetIsSubsetOf,
  SetIsSupersetOf,
  SetIsDisjointFrom,
  DictNew,
  DictGroupBy,
  EnvMap,
  PromiseNew,
  PromiseThen,
  PromiseResolve,
  PromiseAll,
  PromiseRace,
  PromiseWithResolvers,
  SetTimeout,
  SetInterval,
  SetImmediate,
  ClearTimeout,
  ClearImmediate,
  QueueMicrotask,
  NextTick,
  TimerRef,
  TimerUnref,
  TimerHasRef,
  TimerRefresh,
  TimersSetTimeout,
  TimersSetImmediate,
  TimersSetInterval,
  BytesNew,
  BytesFromArray,
  BytesFromBytes,
  BytesOf,
  BytesView,
  BytesByteLength,
  BytesByteOffset,
  BytesBuffer,
  BytesAt,
  BytesIndexOf,
  BytesJoin,
  BytesReverse,
  BytesSort,
  BytesFill,
  BytesSet,
  BytesSubarray,
  BytesSlice,
  BytesCopyWithin,
  BytesToReversed,
  BytesToSorted,
  BytesWith,
  ArrayBufferNew,
  ArrayBufferSlice,
  ArrayBufferIsView,
  DataViewNew,
  DataViewGet,
  DataViewSet,
  BufferFromString,
  BufferAllocFill,
  BufferConcat,
  BufferByteLength,
  BufferCompare,
  BufferCompareTo,
  BufferEquals,
  BufferIsBuffer,
  BufferIsEncoding,
  BufferToString,
  BufferWrite,
  BufferIndexOf,
  BufferCopy,
  BufferFill,
  BufferRead,
  BufferWriteNumber,
  BufferReadVar,
  BufferWriteVar,
  BufferSwap,
  BufferToJSON,
  TextEncoderNew,
  TextEncode,
  TextEncodeInto,
  TextDecoderNew,
  TextDecoderParse,
  TextDecode,
  TextDecoderInfo,
  EmbedBytes,
  Crypto,
  CryptoKeyPair,
  TupleJoin,
  MetaDefine,
  MetaGet,
  MetaGetOwn,
  MetaHas,
  MetaHasOwn,
  MetaDelete,
  LibListener,
  LibAs,
  LibChunk,
  LibIsChunk,
  LibChunkKind,
  LibTypeName,
  LibOverrides,
  LibFsOpen,
  LibFsRead,
  LibFsWrite,
  LibFsClose,
  LibStdinChunk,
  LibStdWrite,
  LibAhead,
  LibOrder,
  LibPanic,
  LibDefer,
  ClassName,
  ReflectGet,
  ReflectApply,
  ReflectConstruct,
  FromJsonClass,
  ReflectMethod,
};

inline bool isLibBuiltin(Builtin builtin) {
  return builtin >= Builtin::LibListener && builtin <= Builtin::LibDefer;
}

inline bool isCryptoBuiltin(Builtin builtin) {
  return builtin == Builtin::Crypto || builtin == Builtin::CryptoKeyPair;
}

inline bool isMetaBuiltin(Builtin builtin) {
  return builtin >= Builtin::MetaDefine && builtin <= Builtin::MetaDelete;
}

inline bool isDictBuiltin(Builtin builtin) {
  return builtin >= Builtin::MapGet && builtin <= Builtin::EnvMap;
}

inline bool isDictIterator(Builtin builtin) {
  return builtin >= Builtin::MapKeys && builtin <= Builtin::SetEntries;
}

inline bool isPathBuiltin(Builtin builtin) {
  return builtin >= Builtin::PathJoin && builtin <= Builtin::PathToNamespacedPath;
}

inline bool isOsBuiltin(Builtin builtin) {
  return builtin >= Builtin::OsString && builtin <= Builtin::OsUserInfo;
}

inline bool isFsModuleBuiltin(Builtin builtin) {
  return builtin >= Builtin::ReadLine && builtin <= Builtin::RlClose;
}

inline bool isFspBuiltin(Builtin builtin) {
  return builtin >= Builtin::FspReadFile && builtin <= Builtin::FhSync;
}

inline bool isChildBuiltin(Builtin builtin) {
  return builtin >= Builtin::ChildSpawnSync && builtin <= Builtin::ChildExecSync;
}

enum ChildArg : unsigned {
  ChildFile,
  ChildArgs,
  ChildCwd,
  ChildEnv,
  ChildInput,
  ChildTimeout,
  ChildKillSignal,
  ChildMaxBuffer,
  ChildShell,
  ChildWindowsHide,
  ChildVerbatim,
  ChildDetached,
  ChildArgv0,
  ChildUid,
  ChildGid,
  ChildStdin,
  ChildStdout,
  ChildStderr,
  ChildStdioGiven,
  ChildEncoding,
  ChildArgCount
};

inline constexpr unsigned ChildPipe = 0, ChildIgnore = 1, ChildInherit = 2, ChildBytes = 0;

inline bool isFsBuiltin(Builtin builtin) {
  return builtin >= Builtin::FsReadFile && builtin <= Builtin::FsRealpath &&
         builtin != Builtin::FsExists;
}

inline bool isPromiseBuiltin(Builtin builtin) {
  return builtin >= Builtin::PromiseNew && builtin <= Builtin::PromiseWithResolvers;
}

inline bool isAsyncBuiltin(Builtin builtin) {
  return builtin >= Builtin::PromiseNew && builtin <= Builtin::TimersSetInterval;
}

inline bool isJsonBuiltin(Builtin builtin) {
  return builtin >= Builtin::JsonParse && builtin <= Builtin::ArrayIsArray;
}

inline bool isDateBuiltin(Builtin builtin) {
  return builtin >= Builtin::DateNew && builtin <= Builtin::DtfResolvedOptions;
}

inline bool isBytesBuiltin(Builtin builtin) {
  return builtin >= Builtin::BytesNew && builtin <= Builtin::EmbedBytes;
}

inline bool isIntlBuiltin(Builtin builtin) {
  return builtin >= Builtin::IntlNew && builtin <= Builtin::SegmentsContaining;
}

inline bool isMathBuiltin(Builtin builtin) {
  return builtin >= Builtin::MathFloor && builtin <= Builtin::MathRandom;
}

inline bool isDecimalBuiltin(Builtin builtin) {
  return builtin >= Builtin::DecimalOf && builtin <= Builtin::DecToFraction;
}

inline bool isResultBuiltin(Builtin builtin) {
  return builtin >= Builtin::ResultOk && builtin <= Builtin::ResultAndThrough;
}

inline bool isResultCallback(Builtin builtin) {
  return builtin >= Builtin::ResultMap && builtin <= Builtin::ResultAndThrough &&
         builtin != Builtin::ResultUnwrapOr;
}

inline bool isRegexBuiltin(Builtin builtin) {
  return builtin >= Builtin::RegExpNew && builtin <= Builtin::StrSplitRegex;
}

inline bool isProcessBuiltin(Builtin builtin) {
  return builtin >= Builtin::ProcessArgv && builtin <= Builtin::PerformanceNow;
}

inline bool isRoundingBuiltin(Builtin builtin) {
  return builtin >= Builtin::MathFloor && builtin <= Builtin::MathTrunc;
}

inline bool isIteratorStage(Builtin builtin) {
  return builtin >= Builtin::IterMap && builtin <= Builtin::IterFlatMap;
}

inline bool isIteratorSink(Builtin builtin) {
  return builtin >= Builtin::IterToArray && builtin <= Builtin::IterFind;
}

}
