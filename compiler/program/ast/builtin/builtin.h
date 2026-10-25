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

// Functions and methods implemented by the compiler and the runtime.
enum class Builtin {
  None,
  ConsoleLog,   // also console.info and console.debug
  ConsoleError, // console.error and console.warn: as console.log, to stderr
  // process, Date.now() and performance.now() (sema_process.cpp). Reads of
  // properties such as `process.argv` become calls without arguments.
  ProcessArgv,
  ProcessEnv,         // process.env.NAME or process.env[name]: the name
  ProcessExit,        // process.exit(code?): `int | undefined`
  ProcessExitCode,    // reading process.exitCode
  ProcessSetExitCode, // `process.exitCode = code`: `int | undefined`
  ProcessCwd,
  ProcessExecPath,
  ProcessPlatform,
  ProcessStdoutWrite, // process.stdout.write(text)
  ProcessStderrWrite,
  ProcessIsTTY, // process.stdout.isTTY or process.stderr.isTTY: the fd
  DateNow,
  PerformanceNow,
  StringOf,      // String(x)
  NumberOf,      // Number(x)
  ParseInt,      // parseInt and Number.parseInt
  ParseFloat,    // parseFloat and Number.parseFloat
  IsNaN,         // isNaN and Number.isNaN
  IsFinite,      // isFinite and Number.isFinite
  IsInteger,     // Number.isInteger
  IsSafeInteger, // Number.isSafeInteger
  FromCharCode,  // String.fromCharCode
  FromCodePoint, // String.fromCodePoint
  // encodeURIComponent, encodeURI, decodeURIComponent, decodeURI.
  UriEncodeComponent,
  UriEncode,
  UriDecodeComponent,
  UriDecode,
  // x.toFixed(digits) of a number.
  NumToFixed,
  // x.toString(radix) of a number.
  NumToRadix,
  // Methods of string; the receiver is the object of the callee MemberExpr.
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
  // Methods of arrays.
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
  // Methods of arrays that take a callback: the first argument.
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
  ArrSortWith, // sort(compare)
  // Object.keys, Object.values and Object.entries of an object: the first
  // argument.
  ObjectKeys,
  ObjectValues,
  ObjectEntries,
  // Iterators (docs/iterators.md). keys(), values() and entries() of an
  // array; Iterator.from(x) and Array.from(x, f?) of an array, a string or an
  // iterator, the first argument.
  ArrKeys,
  ArrValues,
  ArrEntries,
  IterFrom,
  ArrayFrom,
  // `it.next(x?)` of an iterator.
  IterNext,
  // The helpers of iterators that give another iterator: a stage of a lazy
  // chain. The receiver is the source; the argument a callback or a count.
  IterMap,
  IterFilter,
  IterTake,
  IterDrop,
  IterFlatMap,
  // The helpers that consume an iterator.
  IterToArray,
  IterForEach,
  IterReduce,
  IterSome,
  IterEvery,
  IterFind,
  // The functions of Math. The first four round, and give an integer where
  // one is expected (see Sema::checkMathCall).
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
  MathRandom,
  // decimal (sema_decimal.cpp): decimal(x), decimal.max, min and sum, and
  // the methods of a decimal, whose receiver is the object of the callee
  // MemberExpr.
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
  // Result of neverthrow (sema_result.cpp). ok(v) and err(e) take their
  // value; safeTry the generator, which CodeGen calls at once.
  ResultOk,
  ResultErr,
  SafeTry,
  ResultCombine,              // Result.combine(results)
  ResultCombineWithAllErrors, // Result.combineWithAllErrors(results)
  // Methods of Result; the receiver is the object of the callee MemberExpr.
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
  // Result.fromThrowable(f, toError?): a function that calls f and gives
  // what it returns as an Ok, and what it throws as an Err.
  ResultFromThrowable,
  // _unsafeUnwrap() and _unsafeUnwrapErr(), which throw an Error when the
  // Result holds the other half.
  ResultUnsafeUnwrap,
  ResultUnsafeUnwrapErr,
  // Errors (docs/errors.md): reading `e.stack`, and `e.toString()`; the
  // receiver is the object of the callee MemberExpr.
  ErrorStack,
  ErrorToString,
  // Regular expressions (sema_regex.cpp): `new RegExp` of a pattern known
  // only when the program runs, RegExp.escape, the methods and properties of
  // a regex, whose receiver is the object of the callee MemberExpr (reads of
  // properties become calls), and the methods of strings that take a regex
  // as the first argument.
  RegExpNew, // pattern, flags or nothing
  RegExpEscape,
  RegExpTest,
  RegExpExec,
  RegExpToString, // also String(re) and `${re}`, of the first argument
  RegExpSource,
  RegExpFlags,
  RegExpFlag,     // re.global and the like: the flag is the int argument
  StrMatch,       // without g: the first match
  StrMatchGlobal, // with g: the matched texts
  StrMatchAll,
  StrSearch,
  StrReplaceRegex,   // replace or replaceAll with a regex and a string
  StrReplaceRegexFn, // with a function
  StrSplitRegex,
  // JSON and Json values (sema_json.cpp). Reading and writing keys of a Json
  // are member and index expressions of its type.
  JsonParse,     // JSON.parse(text): a Json
  JsonParseAs,   // JSON.parse<T>(text): a T, checked
  FromJson,      // fromJson<T>(j)
  JsonStringify, // JSON.stringify(value, replacer, space): replacer is dropped
  EmbedJson,     // embedJson(path): the arguments are the file's text and its path
  ArrayIsArray,  // Array.isArray(x)
  // Dates (sema_date.cpp, docs/date.md). The numbers of the constructors and
  // of the functions of Date are doubles. The receiver of a method is the
  // object of the callee MemberExpr; a getter or a setter takes the field
  // first, as an int literal (TZ_DATE_* of runtime/core/tzrt/tzrt.h).
  WeakRefNew,     // new WeakRef(object)
  WeakRefDeref,   // w.deref(): the object, or undefined once it died
  DateNew,        // new Date() (now) and new Date(ms)
  DateCopy,       // new Date(date): the date
  DateFromString, // new Date(s): the string
  DateFromFields, // new Date(y, m, d?, h?, min?, s?, ms?), in local time
  DateUTC,        // Date.UTC(y, m?, ...): a number
  DateParse,      // Date.parse(s): a number
  DateText,       // Date(): the text of now, as toString() gives it
  DateGetTime,    // getTime() and valueOf(); also +d, Number(d), `a - b`, `a < b`
  DateGetUTC,     // getUTCFullYear() and the others: the field
  DateGetLocal,   // getFullYear(), ..., getTimezoneOffset(): the field
  DateSetTime,    // setTime(ms)
  DateSetUTC,     // setUTCHours(h, m?, ...) and the others: the field, the values
  DateSetLocal,
  DateToISOString,
  DateToJSON,
  DateToUTCString,
  DateToLocalString, // toString(), toDateString(), toTimeString(): the mode
  DateToLocale,      // toLocaleString() and the like: the kind, the locale?, the options?
  // Intl.DateTimeFormat (docs/date.md, section 3): new takes the locale and
  // the options, as toLocaleString does; format(d) and formatToParts(d) the
  // date, of the formatter that is the receiver.
  DtfNew,
  DtfFormat,
  DtfFormatToParts,
  DtfResolvedOptions,
  // The rest of Intl (sema_intl.cpp, docs/intl.md). The receiver of a method
  // is the object of the callee MemberExpr. The locale and the options, each
  // optional, are the last arguments where there are any.
  IntlNew,              // the kind (TZ_INTL_* of intl.h, an int literal), locale?, options?
  IntlSupportedLocales, // the name of the method (a string literal), locales, options?
  IntlCanonicalLocales, // locales?: a string, a string[] or undefined
  IntlResolvedOptions,
  IntlBound,   // `nf.format` and the like as a value: a function bound to the receiver
  NumToLocale, // x.toLocaleString(locale?, options?) of a number or a decimal
  NfFormat,    // the number: an int, a long, a double or a decimal
  NfFormatToParts,
  CollatorCompare,  // a, b
  StrLocaleCompare, // that, locale?, options?
  StrNormalize,     // form?
  PrSelect,         // the number
  RtfFormat,        // value (a double), unit
  RtfFormatToParts,
  LfFormat, // the list
  LfFormatToParts,
  DnOf,               // the code: string | undefined
  SegSegment,         // the text
  SegmentsArray,      // the segments being iterated, as an array
  SegmentsContaining, // index?
  // The built-in modules (sema_node.cpp, docs/io.md). The functions of
  // node:path take the style first, an int literal: 1 for win32, 0 for posix.
  PathJoin,    // style, the paths as one array
  PathResolve, // style, the paths as one array
  PathNormalize,
  PathIsAbsolute,
  PathDirname,
  PathBasename, // style, path, suffix or nothing
  PathExtname,
  PathRelative,
  PathParse,
  PathFormat, // style, the object
  PathToNamespacedPath,
  // node:os. OsString and OsNumber take which one (an int literal, see
  // runtime/library/node/os/os.c).
  OsString,
  OsNumber,
  OsAvailableParallelism,
  OsCpus,
  OsLoadavg,
  OsUserInfo,
  // readLine(question?) of tz:io.
  ReadLine,
  // node:fs. The path (or the fd, an int, of readFileSync, writeFileSync and
  // appendFileSync) first, then the options Sema took from the call, in the
  // order of the runtime function (see codegen_node.cpp).
  FsReadFile,  // path or fd, flag
  FsWriteFile, // path or fd, data, flag, mode
  FsExists,    // path
  FsAccess,    // path, mode
  FsChmod,     // path, mode
  FsTruncate,  // path, length
  FsStat,      // path, follow links (boolean), throwIfNoEntry (boolean)
  FsReaddir,   // path, recursive; a Dirent[] or a string[]
  FsMkdir,     // path, recursive, mode
  FsMkdtemp,   // prefix
  FsRmdir,     // path
  FsUnlink,    // path
  FsRm,        // path, recursive, force
  FsRename,    // from, to
  FsCopyFile,  // from, to, mode
  FsRealpath,  // path
  // isFile() and the other tests of Stats and Dirent: the receiver is the
  // object of the callee MemberExpr, the kind of file (S_IFREG...) the int
  // argument.
  FsIsKind,
  // `st.mtime` and the other dates of a Stats: the date of the field
  // `mtimeMs`, rounded to the millisecond, as Node makes it. The receiver is
  // the object of the callee MemberExpr.
  FsStatsDate,
  // node:fs/promises (docs/io.md, step 4): those of node:fs above, with the
  // same arguments, that give a Promise; readFile, writeFile and appendFile
  // take an AbortSignal (or undefined) last.
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
  FspOpen, // path, flags, mode: a FileHandle
  // The methods of a FileHandle, whose receiver is the object of the callee
  // MemberExpr; `fh.fd` is a call too.
  FhFd,
  FhClose,
  FhReadFile,  // signal
  FhWriteFile, // data, signal; appendFile too
  FhWrite,     // data, position (-1 for none)
  FhStat,
  FhTruncate, // length
  FhSync,     // sync() and datasync()
  // node:readline and node:readline/promises over stdin. createInterface
  // takes whether the questions are written (to stdout, 1, or stderr, 2;
  // 0 for none) and the signal; the methods take their receiver from the
  // callee MemberExpr.
  RlCreate,
  RlQuestion,         // query, signal: a Promise<string>
  RlQuestionCallback, // query, callback
  RlClose,
  // The synchronous functions of node:child_process (sema_child.cpp,
  // docs/process.md). Sema gives each call the same arguments, in the order
  // of ChildArg: the program and its arguments, the options, then int
  // literals for what decides the types (stdio, encoding).
  ChildSpawnSync,
  ChildExecFileSync,
  ChildExecSync,
  // Dictionaries (sema_dict.cpp): Record, Map and Set. The receiver is the
  // object of the callee MemberExpr; reading and writing the keys of a
  // Record, `delete r[k]` and `k in r` become such calls too.
  MapGet,    // m.get(k) and r[k]: the value, or undefined
  MapSet,    // m.set(k, v): the map
  MapHas,    // m.has(k), s.has(v) and `k in r`
  MapDelete, // m.delete(k), s.delete(v) and `delete r[k]`
  MapClear,
  MapSize,          // m.size and s.size
  SetAdd,           // s.add(v): the set
  RecordSet,        // `r[k] = v`: the value
  RecordNullishSet, // `r[k] ??= v`: the value the key has after it
  MapForEach,       // forEach(f): f(value, key, map), or for a Set f(v, v, set)
  // The iterators of a Map: keys, values and [key, value] entries; of a
  // Set, its elements (values() and keys()) and [v, v] entries.
  MapKeys,
  MapValues,
  MapEntries,
  SetValues,
  SetEntries,
  // The methods of ES2025 that make a Set of two (the other one, a Set or a
  // Map, is the argument), and those that compare them.
  SetUnion,
  SetIntersection,
  SetDifference,
  SetSymmetricDifference,
  SetIsSubsetOf,
  SetIsSupersetOf,
  SetIsDisjointFrom,
  // A new dictionary of the call's type: `new Map(entries)`, `new Set(values)`
  // and Object.fromEntries(entries), from what the argument gives, if any;
  // `{ ...r }` copies.
  DictNew,
  DictGroupBy, // Map.groupBy(items, f) and Object.groupBy(items, f)
  EnvMap,      // process.env as a whole, a Record<string, string>
  // Promises (sema_async.cpp, docs/async.md). `new Promise(executor)`; the
  // methods, whose receiver is the object of the callee MemberExpr; the
  // functions of Promise, which take the value or the iterable of promises.
  PromiseNew,
  PromiseThen,    // then(onFulfilled, onRejected?): both may be undefined
  PromiseCatch,   // catch(onRejected)
  PromiseFinally, // finally(onFinally)
  PromiseResolve, // Promise.resolve(value?)
  PromiseReject,  // Promise.reject(error)
  PromiseAll,
  PromiseAllSettled,
  PromiseRace,
  PromiseAny,
  PromiseWithResolvers,
  // Timers and the queues: the callback first, then the delay.
  SetTimeout,
  SetInterval,
  SetImmediate,
  ClearTimeout, // clearTimeout and clearInterval
  ClearImmediate,
  QueueMicrotask,
  NextTick, // process.nextTick(callback)
  // Methods of a Timeout or an Immediate: ref(), unref(), hasRef(),
  // refresh().
  TimerRef,
  TimerUnref,
  TimerHasRef,
  TimerRefresh,
  // ResultAsync of neverthrow (docs/errors.md): a Promise of a Result. The
  // functions take the Promise and the function that makes the error; the
  // methods, whose receiver is the object of the callee MemberExpr, their
  // callbacks.
  ResultAsyncFromPromise,     // fromPromise(promise, toError)
  ResultAsyncFromSafePromise, // fromSafePromise(promise)
  ResultAsyncFromThrowable,   // ResultAsync.fromThrowable(f, toError?): a function
  ResultAsyncMap,
  ResultAsyncMapErr,
  ResultAsyncAndThen,
  ResultAsyncOrElse,
  ResultAsyncMatch,
  ResultAsyncUnwrapOr,
  ResultAsyncAndTee,
  ResultAsyncOrTee,
  // The parts CodeGen adds: the Promise a callback gave settles the
  // ResultAsync as an Ok or an Err of its value; after the callback of a
  // tee, the Result kept goes on.
  ResultAsyncWrapOk,
  ResultAsyncWrapErr,
  ResultAsyncTeeDone,
  // node:timers/promises (docs/async.md, step 2): setTimeout(delay, value,
  // signal, ref) and setImmediate(value, signal, ref) give a Promise of the
  // value, setInterval(delay, value, signal, ref) an async iterator of it;
  // scheduler.wait(delay, signal) and scheduler.yield().
  TimersSetTimeout,
  TimersSetImmediate,
  TimersSetInterval,
  // Binary data (sema_bytes.cpp, docs/bytes.md). The kind of what a call
  // makes is that of its type; the receiver of a method is the object of the
  // callee MemberExpr. Positions and counts are longs, and an optional one
  // left out is not in args (codegen_bytes.cpp counts them); a choice among
  // variants is an int literal first.
  BytesNew,        // new Uint8Array(n), Buffer.alloc(n) and allocUnsafe(n): n, zeroed (bool)
  BytesFromArray,  // new Uint8Array(array or iterator), Buffer.from(array): the source
  BytesFromBytes,  // new Uint16Array(typedArray), Buffer.from(bytes): a copy, converted
  BytesOf,         // new Uint8Array([1, 2]) and Uint8Array.of(1, 2): the values
  BytesView,       // new Uint8Array(buffer, offset?, length?), Buffer.from(buffer, ...)
  BytesByteLength, // of a typed array, an ArrayBuffer or a DataView
  BytesByteOffset,
  BytesBuffer, // `.buffer` of a typed array or a DataView
  BytesAt,
  BytesIndexOf, // mode (0 indexOf, 1 lastIndexOf, 2 includes), value, from?
  BytesJoin,    // separator?; also toString() and String(u)
  BytesReverse,
  BytesSort,
  BytesFill,       // value, start?, end?
  BytesSet,        // source (array or typed array), offset?
  BytesSubarray,   // start?, end?
  BytesSlice,      // start?, end?
  BytesCopyWithin, // target, start, end?
  BytesToReversed,
  BytesToSorted,
  BytesWith,      // index, value
  ArrayBufferNew, // length
  ArrayBufferSlice,
  ArrayBufferIsView,
  DataViewNew,       // buffer, offset?, length?
  DataViewGet,       // kind (TZ_BYTES_*), offset, littleEndian?
  DataViewSet,       // kind, offset, value, littleEndian?
  BufferFromString,  // string, encoding (an int literal, or a string)
  BufferAllocFill,   // size, fill (number, string or bytes), encoding?
  BufferConcat,      // list, totalLength?
  BufferByteLength,  // string, encoding
  BufferCompare,     // Buffer.compare(a, b)
  BufferCompareTo,   // buf.compare(target, targetStart?, targetEnd?, sourceStart?, sourceEnd?)
  BufferEquals,      // other
  BufferIsBuffer,    // x, whose type tells
  BufferIsEncoding,  // name
  BufferToString,    // encoding?, start?, end?
  BufferWrite,       // string, offset?, length?, encoding?
  BufferIndexOf,     // mode, value (number, string or bytes), byteOffset?, encoding?
  BufferCopy,        // target, targetStart?, sourceStart?, sourceEnd?
  BufferFill,        // value (number, string or bytes), offset?, end?, encoding?
  BufferRead,        // kind (TZ_BYTES_*), big-endian (bool), offset?
  BufferWriteNumber, // kind, big-endian, value, offset?
  BufferReadVar,     // signed (bool), big-endian, offset, byteLength
  BufferWriteVar,    // signed, big-endian, value, offset, byteLength
  BufferSwap,        // size (2, 4 or 8)
  BufferToJSON,
  TextEncoderNew,
  TextEncode,      // string?
  TextEncodeInto,  // string, destination
  TextDecoderNew,  // label?, options?: the label is an int (the encoding Sema found) or a string
  TextDecode,      // input?
  TextDecoderInfo, // 0 encoding, 1 fatal, 2 ignoreBOM
  EmbedBytes,      // embedBytes(path): the bytes (one per code unit) and the path
  // node:crypto (sema_crypto.cpp, docs/crypto.md): a call of a function of
  // runtime/library/crypto/, whose number (CryptoFn of crypto_api.h) is the
  // first argument, an int literal, and the receiver of a method the second.
  Crypto,
  // generateKeyPairSync: the call that makes the private key, then for the
  // public and the private key the format of its encoding (-1 for a
  // KeyObject), the options given, the type, the cipher and the passphrase.
  CryptoKeyPair,
  // `t.join(sep?)` of a tuple, as a list of parameters (docs/generics.md,
  // section 9) is one: the receiver is the object of the callee MemberExpr.
  TupleJoin,
  // Metadata (docs/decorators.md, section 3): Reflect.defineMetadata and
  // the others take the key (a string; an empty one for the key
  // CallExpr::metaKey), for define the value as an unknown, the target as an
  // object, and the property as a `string | undefined`.
  MetaDefine,
  MetaGet,
  MetaGetOwn,
  MetaHas,
  MetaHasOwn,
  MetaDelete,
  // The intrinsics of the built-in modules written in tz (library/, see
  // sema_events.cpp) that need the compiler, as they take a type:
  // `__tz_listener<F>(u)`, the function an unknown holds, as an F;
  // `__tz_as<C>(o)`, the object o as an instance of the class C, or undefined
  // if it is not one. The others are declared there, `function __tz_x(...)`
  // without a body, calls of the runtime's tz_x (sema_library.cpp). Then
  // what Sema makes of the `signal` of a call (signalOption): the runtime's
  // side of the AbortSignal, or undefined.
  LibListener,
  LibAs,
  // node:stream (compiler/library/stream/sema/sema_stream.cpp):
  // `__tz_chunk<T>(u)`, the T the unknown u holds; `__tz_isChunk<T>(u)`,
  // whether it holds one; `__tz_chunkKind<T>()` and `__tz_typeName<T>()`,
  // constants of each copy; `__tz_overrides(o, "m")`, whether the class of the
  // object o replaces the method m of the class the code knows it by.
  LibChunk,
  LibIsChunk,
  LibChunkKind,
  LibTypeName,
  LibOverrides,
  // The I/O of the streams of node:fs and process
  // (library/stream/fs_stream.tz), each a Promise but the last:
  // `__tz_fsOpen(path, flags, mode)` of the descriptor, `__tz_fsRead(fd,
  // buffer, offset, length, position)` and `__tz_fsWrite(...)` of the bytes
  // moved, `__tz_fsClose(fd, fsync)`, `__tz_stdinChunk(max)` of the next Buffer
  // of stdin (undefined at its end), and `__tz_stdWrite(fd, buffer)`, which
  // writes to stdout or stderr.
  LibFsOpen,
  LibFsRead,
  LibFsWrite,
  LibFsClose,
  LibStdinChunk,
  LibStdWrite,
  // The parallel runtime (runtime/core/parallel/parallel.c), for code of the
  // built-in modules that the analysis of compiler/parallel/parallel_analysis/parallel_analysis.h
  // does not walk: `await __tz_ahead(p)`, an await after which the piece may
  // run ahead of its turn, which the code itself guarantees, and
  // `__tz_order()`, where it waits for its turn before it touches shared data.
  LibAhead,
  LibOrder,
  LibNativeSignal,
  // Reflection (docs/decorators.md, section 6), through the descriptions of
  // the classes: the name of a class known only when the program runs (its
  // object); Reflect.get(object, name), Reflect.apply(f, this, args) and
  // Reflect.construct(cls, args), with unknowns for values; fromJson(cls,
  // j), the object a class makes from a Json through its constructor.
  ClassName,
  ReflectGet,
  ReflectApply,
  ReflectConstruct,
  FromJsonClass,
  // Reflect.get(object, "m") of an object of a class known when compiling,
  // whose method `method` no class down its chain declares again: the
  // function that takes its `this`, as an unknown, without the description of
  // the class. The object is evaluated and dropped.
  ReflectMethod,
};

inline bool isLibBuiltin(Builtin builtin) {
  return builtin >= Builtin::LibListener && builtin <= Builtin::LibNativeSignal;
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

// The iterators of dictionaries.
inline bool isDictIterator(Builtin builtin) {
  return builtin >= Builtin::MapKeys && builtin <= Builtin::SetEntries;
}

inline bool isNodeBuiltin(Builtin builtin) {
  return builtin >= Builtin::PathJoin && builtin <= Builtin::RlClose;
}

// The functions of node:fs/promises and the methods of FileHandle, which
// give a Promise.
inline bool isFspBuiltin(Builtin builtin) {
  return builtin >= Builtin::FspReadFile && builtin <= Builtin::FhSync;
}

// spawnSync, execFileSync and execSync of node:child_process.
inline bool isChildBuiltin(Builtin builtin) {
  return builtin >= Builtin::ChildSpawnSync && builtin <= Builtin::ChildExecSync;
}

// The arguments Sema gives a call of one of them. The program (the command
// of execSync) is a string and its arguments a string[]; each option is
// `T | undefined`, undefined when the call does not give it: input a string
// or a view of bytes, killSignal a string or an int, shell a boolean or a
// string. The int literals last: what each of stdin, stdout and stderr is
// (ChildPipe, ChildIgnore, or ChildInherit plus the fd the child inherits),
// whether the call gave `stdio`, and the encoding of the output (ChildBytes
// for Buffers, or one plus the number of a Buffer encoding).
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

// The functions of node:fs, which throw errors of the system.
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

inline bool isResultAsyncMethod(Builtin builtin) {
  return builtin >= Builtin::ResultAsyncMap && builtin <= Builtin::ResultAsyncOrTee;
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
  return builtin >= Builtin::ResultOk && builtin <= Builtin::ResultUnsafeUnwrapErr;
}

// The methods of Result that call a function given to them.
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

// A helper that makes a stage of a lazy chain: map, filter, take, drop,
// flatMap.
inline bool isIteratorStage(Builtin builtin) {
  return builtin >= Builtin::IterMap && builtin <= Builtin::IterFlatMap;
}

// A helper that consumes an iterator: toArray, forEach, reduce, some, every,
// find.
inline bool isIteratorSink(Builtin builtin) {
  return builtin >= Builtin::IterToArray && builtin <= Builtin::IterFind;
}

} // namespace tz
