// What tz has built in, declared for TypeScript's checker in the editor (tsc in
// VS Code): the names a program uses without an import, and the modules
// "tz:io" and "tz:http" (docs/modules.md, section 6). tz itself does not read
// this file; tz build checks the program by its own rules.
//
// It lies next to tz as types/node_modules/@types/tz/index.d.ts, and
// types/tsconfig.json (editors/types/tsconfig.json) brings it in together with
// @types/node: for a plugin of tsserver that would give the
// .tz files of the editor to TypeScript's checker. There is none yet;
// tests/cli/types.cmake checks these declarations on .ts copies of programs.
//
// Where tz knows more than a declaration can say, the declaration is looser
// than tz, so that the editor does not underline what tz accepts: integers
// and decimal are number here, and the value of metadata is any. Where
// TypeScript itself is stricter than tz (Json without a check, overloads with
// a body each), the editor underlines code that builds; tz's own language
// server will not.

// Numbers (docs/numbers.md, docs/decimal.md). For tsc all are number; tz tells
// them apart and checks what mixes them.
type int = number
type int8 = number
type int16 = number
type int32 = number
type uint8 = number
type uint16 = number
type uint32 = number
type uint64 = number
type double = number
type decimal = number

// Rounding modes of decimal (docs/decimal.md, section 4). tz checks a literal
// mode when it compiles the program; a mode in a variable is any string.
type DecimalRoundingMode =
  | "up" | "down" | "ceil" | "floor"
  | "half-up" | "half-down" | "half-even" | "half-ceil" | "half-floor"
  | (string & {})

// decimal("0.1"), decimal(x): an exact decimal from a string or a number. A
// string known when tz builds the program (a literal, a constant) is checked
// then and gives a decimal; any other string gives a Result.
type DecimalError = { code: "bad_decimal"; message: string; text: string }
declare function decimal<S extends string>(
  value: S,
): string extends S ? Result<decimal, DecimalError> : decimal
declare function decimal(value: number): decimal
declare namespace decimal {
  function max(...values: decimal[]): decimal
  function min(...values: decimal[]): decimal
  function sum(...values: decimal[]): decimal
}

// The methods of a decimal. Since decimal is number here, numbers get them in
// the editor too; tz rejects them on a double. The conversions of numbers
// (docs/numbers.md, section 3) come first.
interface Number {
  toInt(): int
  toInt8(): int8
  toInt16(): int16
  toInt32(): int32
  toUint8(): uint8
  toUint16(): uint16
  toUint32(): uint32
  toUint64(): uint64
  toDouble(): double
  toDecimal(): decimal
  // The bits of a Bit.Wrap32 or Bit.Wrap64.
  readonly value: number
  div(divisor: decimal, places?: int, mode?: DecimalRoundingMode): decimal
  round(places?: int, mode?: DecimalRoundingMode): decimal
  toFixed(places?: int, mode?: DecimalRoundingMode): string
  toFormat(places?: int, mode?: DecimalRoundingMode): string
  toExponential(places?: int, mode?: DecimalRoundingMode): string
  toPrecision(digits?: int, mode?: DecimalRoundingMode): string
  sqrt(): decimal
  abs(): decimal
  isInteger(): boolean
  decimalPlaces(): int
  shiftedBy(power: int): decimal
  toFraction(maxDenominator?: decimal): decimal[]
}

// Numbers from strings give a Result (docs/numbers.md, section 9). tsc keeps its
// own global parseInt and parseFloat, which come first.
type NumberError = { code: "bad_number"; message: string }
interface NumberConstructor {
  (value: string): Result<number, NumberError>
  parseInt(text: string, radix?: number): Result<int, NumberError>
  parseFloat(text: string): Result<number, NumberError>
}

// No NaN and no Infinity (docs/numbers.md, section 10): Math.clamp, and a double
// read from bytes gives a Result. tsc keeps its own NaN, Infinity, isNaN and
// isFinite, which tz does not have.
interface Math {
  clamp(x: number, lo: number, hi: number): number
}
interface DataView {
  getFloat16(byteOffset: number, littleEndian?: boolean): Result<number, NumberError>
  getFloat32(byteOffset: number, littleEndian?: boolean): Result<number, NumberError>
  getFloat64(byteOffset: number, littleEndian?: boolean): Result<number, NumberError>
}
interface Buffer {
  readFloatLE(offset?: number): Result<number, NumberError>
  readFloatBE(offset?: number): Result<number, NumberError>
  readDoubleLE(offset?: number): Result<number, NumberError>
  readDoubleBE(offset?: number): Result<number, NumberError>
}

// Counting modulo 2^32 and 2^64, and what is one instruction of the processor
// (docs/numbers.md, sections 7 and 8).
declare namespace Bit {
  type Wrap32 = number
  type Wrap64 = number
  const Wrap32: { from(x: number): Wrap32 }
  const Wrap64: { from(x: number): Wrap64 }
  function rotateLeft32(x: number, n: number): number
  function rotateLeft64(x: number, n: number): number
  function rotateRight32(x: number, n: number): number
  function rotateRight64(x: number, n: number): number
  function leadingZeros32(x: number): int
  function leadingZeros64(x: number): int
  function trailingZeros32(x: number): int
  function trailingZeros64(x: number): int
  function onesCount32(x: number): int
  function onesCount64(x: number): int
  function reverseBytes32(x: number): number
  function reverseBytes64(x: number): number
  function mul64(a: uint64, b: uint64): { hi: uint64; lo: uint64 }
  function add64(a: uint64, b: uint64, carry: uint64): { sum: uint64; carry: uint64 }
  function sub64(a: uint64, b: uint64, borrow: uint64): { diff: uint64; borrow: uint64 }
}

// JSON (docs/modules.md, section 5; docs/narrowing.md, section 3).
type Json = null | boolean | number | string | Json[] | { [key: string]: Json }
type JsonError = { code: "bad_json" | "bad_field" | "bad_class" | "circular"; message: string; path: string }
type UriError = { code: "bad_uri"; message: string }
type RegexError = { code: "bad_regex"; message: string }
type UrlError = { code: "bad_url"; message: string; input: string }
type DateError = { code: "bad_date"; message: string; text: string }
type LocaleError = { code: "bad_locale"; message: string; tag: string }
type DecodeError = { code: "ERR_ENCODING_INVALID_ENCODED_DATA"; message: string }
type EncodingError = { code: "ERR_ENCODING_NOT_SUPPORTED"; message: string }

// Date.parse gives the milliseconds, or an Err for a text it cannot read
// (docs/date.md, section 4).
interface DateConstructor {
  parse(text: string): Result<int, DateError>
}

// The web (docs/net.md): fetch, the reading of a body, URL.parse and the
// reading of a stream give a Result; bodyUsed() and desiredSize() are
// methods; the status of a Response is an HttpStatus. A declaration here
// cannot change what @types/node declares, so tz changes those declarations in
// its copy of @types/node (editors/types/patch.cmake).
type FetchError = { code: string; message: string; cause?: string }
type BodyError = {
  code: "body_used" | "terminated" | "bad_form" | "body_failed" | "ABORT_ERR" | "TIMEOUT_ERR"
  message: string
  cause?: string
}
// The statuses a Response may have; a status from a variable becomes one
// after a check: if (isHttpStatus(code)).
type HttpStatus =
  | 200 | 201 | 202 | 203 | 204 | 205 | 206 | 207 | 208 | 209 | 210 | 211 | 212 | 213 | 214 | 215 | 216 | 217 | 218 | 219
  | 220 | 221 | 222 | 223 | 224 | 225 | 226 | 227 | 228 | 229 | 230 | 231 | 232 | 233 | 234 | 235 | 236 | 237 | 238 | 239
  | 240 | 241 | 242 | 243 | 244 | 245 | 246 | 247 | 248 | 249 | 250 | 251 | 252 | 253 | 254 | 255 | 256 | 257 | 258 | 259
  | 260 | 261 | 262 | 263 | 264 | 265 | 266 | 267 | 268 | 269 | 270 | 271 | 272 | 273 | 274 | 275 | 276 | 277 | 278 | 279
  | 280 | 281 | 282 | 283 | 284 | 285 | 286 | 287 | 288 | 289 | 290 | 291 | 292 | 293 | 294 | 295 | 296 | 297 | 298 | 299
  | 300 | 301 | 302 | 303 | 304 | 305 | 306 | 307 | 308 | 309 | 310 | 311 | 312 | 313 | 314 | 315 | 316 | 317 | 318 | 319
  | 320 | 321 | 322 | 323 | 324 | 325 | 326 | 327 | 328 | 329 | 330 | 331 | 332 | 333 | 334 | 335 | 336 | 337 | 338 | 339
  | 340 | 341 | 342 | 343 | 344 | 345 | 346 | 347 | 348 | 349 | 350 | 351 | 352 | 353 | 354 | 355 | 356 | 357 | 358 | 359
  | 360 | 361 | 362 | 363 | 364 | 365 | 366 | 367 | 368 | 369 | 370 | 371 | 372 | 373 | 374 | 375 | 376 | 377 | 378 | 379
  | 380 | 381 | 382 | 383 | 384 | 385 | 386 | 387 | 388 | 389 | 390 | 391 | 392 | 393 | 394 | 395 | 396 | 397 | 398 | 399
  | 400 | 401 | 402 | 403 | 404 | 405 | 406 | 407 | 408 | 409 | 410 | 411 | 412 | 413 | 414 | 415 | 416 | 417 | 418 | 419
  | 420 | 421 | 422 | 423 | 424 | 425 | 426 | 427 | 428 | 429 | 430 | 431 | 432 | 433 | 434 | 435 | 436 | 437 | 438 | 439
  | 440 | 441 | 442 | 443 | 444 | 445 | 446 | 447 | 448 | 449 | 450 | 451 | 452 | 453 | 454 | 455 | 456 | 457 | 458 | 459
  | 460 | 461 | 462 | 463 | 464 | 465 | 466 | 467 | 468 | 469 | 470 | 471 | 472 | 473 | 474 | 475 | 476 | 477 | 478 | 479
  | 480 | 481 | 482 | 483 | 484 | 485 | 486 | 487 | 488 | 489 | 490 | 491 | 492 | 493 | 494 | 495 | 496 | 497 | 498 | 499
  | 500 | 501 | 502 | 503 | 504 | 505 | 506 | 507 | 508 | 509 | 510 | 511 | 512 | 513 | 514 | 515 | 516 | 517 | 518 | 519
  | 520 | 521 | 522 | 523 | 524 | 525 | 526 | 527 | 528 | 529 | 530 | 531 | 532 | 533 | 534 | 535 | 536 | 537 | 538 | 539
  | 540 | 541 | 542 | 543 | 544 | 545 | 546 | 547 | 548 | 549 | 550 | 551 | 552 | 553 | 554 | 555 | 556 | 557 | 558 | 559
  | 560 | 561 | 562 | 563 | 564 | 565 | 566 | 567 | 568 | 569 | 570 | 571 | 572 | 573 | 574 | 575 | 576 | 577 | 578 | 579
  | 580 | 581 | 582 | 583 | 584 | 585 | 586 | 587 | 588 | 589 | 590 | 591 | 592 | 593 | 594 | 595 | 596 | 597 | 598 | 599
type RedirectStatus = 301 | 302 | 303 | 307 | 308
declare function isHttpStatus(code: number): code is HttpStatus

// The parts of a URL are fields that only its methods change (docs/objects.md,
// section 6); @types/node still lets the fields be assigned. A value that the
// URL can refuse gives a Result; tz checks a literal when it builds the program.
declare module "url" {
  interface URL {
    setHref(value: string): Result<void, UrlError>
    setProtocol(value: string): Result<void, UrlError>
    setHost(value: string): Result<void, UrlError>
    setHostname(value: string): Result<void, UrlError>
    setPort(value: string): Result<void, UrlError>
    setUsername(value: string): void
    setPassword(value: string): void
    setPathname(value: string): void
    setSearch(value: string): void
    setHash(value: string): void
  }

  interface URLSearchParams {
    watch(watcher: (query?: string) => void): void
    replace(query?: string): void
  }
}

// What the objects of the web platform give in tz besides their API
// (docs/objects.md, section 6): the parameters of a URL tell those who watch
// them what changed and are set from a text with replace; an event is sent
// out through dispatchOn and tells whether stopImmediatePropagation() was
// called; a stream tells whether it was read; a key does its own operations,
// which crypto.subtle calls. AbortSignal.withAbort(), which gives a signal
// together with the function that aborts it, cannot be added here to the
// AbortSignal of @types/node, which is a var.
interface Event {
  readonly immediateStopped: boolean
  dispatchOn(target: EventTarget, run: () => void): boolean
}

declare module "stream/web" {
  interface ReadableStream<R = any> {
    readonly disturbed: boolean
  }
}

declare module "crypto" {
  namespace webcrypto {
    interface CryptoKey {
      sign(algorithm: AlgorithmIdentifier, data: BufferSource): Promise<Result<ArrayBuffer, SubtleError>>
      verify(algorithm: AlgorithmIdentifier, signature: BufferSource,
             data: BufferSource): Promise<Result<boolean, SubtleError>>
      encrypt(algorithm: AlgorithmIdentifier, data: BufferSource): Promise<Result<ArrayBuffer, SubtleError>>
      decrypt(algorithm: AlgorithmIdentifier, data: BufferSource): Promise<Result<ArrayBuffer, SubtleError>>
      deriveBits(algorithm: AlgorithmIdentifier, length?: number): Promise<Result<ArrayBuffer, SubtleError>>
      deriveKey(algorithm: AlgorithmIdentifier, derivedKeyAlgorithm: AlgorithmIdentifier,
                extractable: boolean, usages: KeyUsage[]): Promise<Result<CryptoKey, SubtleError>>
      wrap(format: KeyFormat, key: CryptoKey,
           algorithm: AlgorithmIdentifier): Promise<Result<ArrayBuffer, SubtleError>>
      unwrap(format: KeyFormat, wrappedKey: BufferSource,
             algorithm: AlgorithmIdentifier): Promise<Result<ArrayBuffer | JsonWebKey, SubtleError>>
      export(format: KeyFormat): Promise<Result<ArrayBuffer | JsonWebKey, SubtleError>>
    }
  }
}

interface RegExpConstructor {
  parse(source: string, flags?: string): Result<RegExp, RegexError>
}

interface JSON {
  // JSON.parse<User>(text) checks every field and gives a Result with a User.
  // Without a type argument the rest parameter wants a value of type never,
  // so this declaration does not fit and tsc takes its own JSON.parse.
  parse<T = never>(
    text: string,
    ...noTypeArgument: [T] extends [never] ? [never] : []
  ): Result<T, JsonError>
}

// A Json value as type T, every field checked; with a class, made by its
// constructor.
declare function fromJson<T>(json: Json): Result<T, JsonError>
declare function fromJson<T>(
  cls: abstract new (...args: never[]) => T,
  json: Json,
): Result<T, JsonError>

// Files built into the executable, read when the program is compiled; the
// path is relative to the file of the call.
declare function embedJson(path: string): Json
declare function embedText(path: string): string
declare function embedBytes(path: string): Buffer

// The names of the web platform that tz has as globals and @types/node only
// inside its modules, or only as values.
interface EventListenerObject {
  handleEvent(event: Event): void
}
type HeadersInit = import("undici-types").HeadersInit
type BodyInit = import("undici-types").BodyInit
type RequestRedirect = import("undici-types").RequestRedirect
type ReadableStreamReadResult<T> = import("node:stream/web").ReadableStreamReadResult<T>
type UnderlyingSource<R> = import("node:stream/web").UnderlyingSource<R>
type CryptoKey = import("node:crypto").webcrypto.CryptoKey
type CryptoKeyPair = import("node:crypto").webcrypto.CryptoKeyPair
type SubtleCrypto = import("node:crypto").webcrypto.SubtleCrypto
type JsonWebKey = import("node:crypto").webcrypto.JsonWebKey
type RsaOtherPrimesInfo = import("node:crypto").webcrypto.RsaOtherPrimesInfo
type BufferSource = import("node:crypto").webcrypto.BufferSource
type KeyUsage = import("node:crypto").webcrypto.KeyUsage
type KeyFormat = import("node:crypto").webcrypto.KeyFormat
type KeyType = import("node:crypto").webcrypto.KeyType
type NamedCurve = import("node:crypto").webcrypto.NamedCurve
type BigInteger = import("node:crypto").webcrypto.BigInteger
type Algorithm = import("node:crypto").webcrypto.Algorithm
type AlgorithmIdentifier = import("node:crypto").webcrypto.AlgorithmIdentifier
type HashAlgorithmIdentifier = import("node:crypto").webcrypto.HashAlgorithmIdentifier
type KeyAlgorithm = import("node:crypto").webcrypto.KeyAlgorithm
type AesKeyAlgorithm = import("node:crypto").webcrypto.AesKeyAlgorithm
type HmacKeyAlgorithm = import("node:crypto").webcrypto.HmacKeyAlgorithm
type EcKeyAlgorithm = import("node:crypto").webcrypto.EcKeyAlgorithm
type RsaKeyAlgorithm = import("node:crypto").webcrypto.RsaKeyAlgorithm
type RsaHashedKeyAlgorithm = import("node:crypto").webcrypto.RsaHashedKeyAlgorithm
type AesCbcParams = import("node:crypto").webcrypto.AesCbcParams
type AesCtrParams = import("node:crypto").webcrypto.AesCtrParams
type AesGcmParams = import("node:crypto").webcrypto.AesGcmParams
type AesKeyGenParams = import("node:crypto").webcrypto.AesKeyGenParams
type AesDerivedKeyParams = import("node:crypto").webcrypto.AesDerivedKeyParams
type HmacImportParams = import("node:crypto").webcrypto.HmacImportParams
type HmacKeyGenParams = import("node:crypto").webcrypto.HmacKeyGenParams
type EcKeyGenParams = import("node:crypto").webcrypto.EcKeyGenParams
type EcKeyImportParams = import("node:crypto").webcrypto.EcKeyImportParams
type EcdsaParams = import("node:crypto").webcrypto.EcdsaParams
type EcdhKeyDeriveParams = import("node:crypto").webcrypto.EcdhKeyDeriveParams
type RsaKeyGenParams = import("node:crypto").webcrypto.RsaKeyGenParams
type RsaHashedKeyGenParams = import("node:crypto").webcrypto.RsaHashedKeyGenParams
type RsaHashedImportParams = import("node:crypto").webcrypto.RsaHashedImportParams
type RsaPssParams = import("node:crypto").webcrypto.RsaPssParams
type RsaOaepParams = import("node:crypto").webcrypto.RsaOaepParams
type HkdfParams = import("node:crypto").webcrypto.HkdfParams

// What setTimeout and setImmediate give.
type Timeout = NodeJS.Timeout
type Immediate = NodeJS.Immediate

// An error of the system: a file that is not there, no access, a port that
// is taken (docs/io.md, section 5). Node has no such global; tz does.
type SubtleError = {
  code: "NotSupportedError" | "DataError" | "OperationError" | "InvalidAccessError" |
    "SyntaxError" | "TypeError" | "RangeError"
  message: string
}

type SystemError = {
  code: string
  message: string
  errno: int
  syscall: string
  path?: string
  dest?: string
}

// The error of a stream: what destroy(e) is given, the Err of for await,
// pipeline, finished and end() (docs/streams.md).
type StreamError = {
  code: string
  message: string
  errno?: int
  syscall?: string
  path?: string
  cause?: string
}

// The error of once(), on() and the timers of node:timers/promises that a
// signal ends.
type AbortError = { code: "ABORT_ERR"; message: string }

// The error of execSync, execFileSync and spawnSync of node:child_process
// (docs/process.md): the program could not start, ran too long or printed
// too much (the code of the system), or ended badly (the code "exit").
type ExecError<Out> = {
  code: string
  message: string
  errno?: int
  syscall?: string
  path?: string
  spawnargs?: string[]
  status?: int
  signal?: string
  stdout?: Out
  stderr?: Out
}

// Decorators and reflection (docs/decorators.md). A class whose objects are
// T. tsc lets a class extend a value of a type parameter only when its
// constructor takes `...args: any[]`, so the any here is tsc's rule.
type Constructor<T = object> = new (...args: any[]) => T
// The parameters of the constructor of the class a decorator replaces.
// The any is tsc's rule again: the constructor of such a class takes
// `...args: ConstructorArgs` and passes them to super.
type ConstructorArgs = any[]
// What a decorator of a method gets: A are its parameters, R its result.
type MethodDescriptor<A extends unknown[], R> = TypedPropertyDescriptor<(...args: A) => R>

// Metadata, as the package reflect-metadata has it, built in. tz gives the
// value of a key the type of what the program writes under it; tsc cannot,
// so it gets any, as with reflect-metadata.
declare namespace Reflect {
  function defineMetadata(key: unknown, value: unknown, target: object, member?: string | symbol): void
  function getMetadata(key: unknown, target: object, member?: string | symbol): any
  function getOwnMetadata(key: unknown, target: object, member?: string | symbol): any
  function hasMetadata(key: unknown, target: object, member?: string | symbol): boolean
  function hasOwnMetadata(key: unknown, target: object, member?: string | symbol): boolean
  function deleteMetadata(key: unknown, target: object, member?: string | symbol): boolean
  function metadata(key: unknown, value: unknown): {
    (target: object): void
    (target: object, member: string | symbol, descriptor?: PropertyDescriptor): void
  }
}

// Result (docs/errors.md, section 1): built in, no import.
interface ResultMethods<T, E> {
  isOk(): this is Ok<T, E>
  isErr(): this is Err<T, E>
  map<U>(f: (value: T) => U): Result<U, E>
  mapErr<F>(f: (error: E) => F): Result<T, F>
  andThen<U, F>(f: (value: T) => Result<U, F>): Result<U, E | F>
  orElse<U, F>(f: (error: E) => Result<U, F>): Result<T | U, F>
  match<A, B = A>(ok: (value: T) => A, err: (error: E) => B): A | B
  unwrapOr<U>(fallback: U): T | U
  andTee(f: (value: T) => unknown): Result<T, E>
  orTee(f: (error: E) => unknown): Result<T, E>
  andThrough<F>(f: (value: T) => Result<unknown, F>): Result<T, E | F>
}
interface Ok<T, E> extends ResultMethods<T, E> {
  readonly value: T
}
interface Err<T, E> extends ResultMethods<T, E> {
  readonly error: E
}
type Result<T, E> = Ok<T, E> | Err<T, E>
type ResultValue<R> = R extends Ok<infer T, unknown> | Err<infer T, unknown> ? T : never
type ResultError<R> = R extends Ok<unknown, infer E> | Err<unknown, infer E> ? E : never
type ResultValues<R extends readonly Result<unknown, unknown>[]> = {
  -readonly [K in keyof R]: ResultValue<R[K]>
}
declare function ok<T, E = never>(value: T): Ok<T, E>
declare function ok<T extends void = void, E = never>(value: void): Ok<void, E>
declare function err<T = never, E extends string = string>(error: E): Err<T, E>
declare function err<T = never, E = unknown>(error: E): Err<T, E>
declare function err<T = never, E extends void = void>(error: void): Err<T, void>
declare const Result: {
  combine<R extends readonly Result<unknown, unknown>[]>(
    results: readonly [...R],
  ): Result<ResultValues<R>, ResultError<R[number]>>
  combineWithAllErrors<R extends readonly Result<unknown, unknown>[]>(
    results: readonly [...R],
  ): Result<ResultValues<R>, ResultError<R[number]>[]>
}

// node:fs, node:fs/promises and node:child_process give a Result where node
// throws (docs/io.md, section 5; docs/process.md): these names of the
// modules hide the ones of @types/node.
declare module "node:fs" {
  import type { Dirent, Stats } from "fs"
  type Path = string | Buffer | URL
  type File = Path | number
  type Data = string | NodeJS.ArrayBufferView
  type Flag =
    | "r" | "rs" | "sr" | "r+" | "rs+" | "sr+"
    | "w" | "wx" | "xw" | "w+" | "wx+" | "xw+"
    | "a" | "ax" | "xa" | "as" | "sa" | "a+" | "ax+" | "xa+" | "as+" | "sa+"
  type Done<T> = Result<T, SystemError>
  export function readFileSync(path: File, options?: { encoding?: undefined; flag?: Flag }): Done<Buffer>
  export function readFileSync(path: File, options: BufferEncoding | { encoding: BufferEncoding; flag?: Flag }): Done<string>
  export function writeFileSync(file: File, data: Data, options?: BufferEncoding | { encoding?: BufferEncoding; flag?: Flag; mode?: number }): Done<void>
  export function appendFileSync(file: File, data: Data, options?: BufferEncoding | { encoding?: BufferEncoding; flag?: Flag; mode?: number }): Done<void>
  export function accessSync(path: Path, mode?: number): Done<void>
  export function chmodSync(path: Path, mode: number | string): Done<void>
  export function truncateSync(path: Path, len?: number): Done<void>
  export function statSync(path: Path, options: { throwIfNoEntry: false }): Done<Stats | undefined>
  export function statSync(path: Path, options?: { throwIfNoEntry?: true }): Done<Stats>
  export function lstatSync(path: Path, options: { throwIfNoEntry: false }): Done<Stats | undefined>
  export function lstatSync(path: Path, options?: { throwIfNoEntry?: true }): Done<Stats>
  export function readdirSync(path: Path, options: { withFileTypes: true; recursive?: boolean }): Done<Dirent[]>
  export function readdirSync(path: Path, options?: "utf8" | { encoding?: "utf8"; withFileTypes?: false; recursive?: boolean }): Done<string[]>
  export function mkdirSync(path: Path, options: { recursive: true; mode?: number }): Done<string | undefined>
  export function mkdirSync(path: Path, options?: number | { recursive?: false; mode?: number }): Done<void>
  export function mkdtempSync(prefix: string, options?: "utf8" | { encoding?: "utf8" }): Done<string>
  export function rmdirSync(path: Path): Done<void>
  export function unlinkSync(path: Path): Done<void>
  export function rmSync(path: Path, options?: { recursive?: boolean; force?: boolean; maxRetries?: number; retryDelay?: number }): Done<void>
  export function renameSync(oldPath: Path, newPath: Path): Done<void>
  export function copyFileSync(src: Path, dest: Path, mode?: number): Done<void>
  export function realpathSync(path: Path, options?: "utf8" | { encoding?: "utf8" }): Done<string>
  export const promises: typeof import("node:fs/promises")
}

declare module "node:child_process" {
  import type { ExecFileSyncOptions, ExecSyncOptions, SpawnSyncOptions, SpawnSyncReturns } from "child_process"
  type Text = { encoding: BufferEncoding }
  type Bytes = { encoding?: "buffer" }
  type Ran<T> = Omit<SpawnSyncReturns<T>, "error">
  export function execSync(command: string, options: ExecSyncOptions & Text): Result<string, ExecError<string>>
  export function execSync(command: string, options?: ExecSyncOptions & Bytes): Result<Buffer, ExecError<Buffer>>
  export function execFileSync(file: string, options: ExecFileSyncOptions & Text): Result<string, ExecError<string>>
  export function execFileSync(file: string, args: readonly string[], options: ExecFileSyncOptions & Text): Result<string, ExecError<string>>
  export function execFileSync(file: string, options?: ExecFileSyncOptions & Bytes): Result<Buffer, ExecError<Buffer>>
  export function execFileSync(file: string, args?: readonly string[], options?: ExecFileSyncOptions & Bytes): Result<Buffer, ExecError<Buffer>>
  export function spawnSync(command: string, options: SpawnSyncOptions & Text): Result<Ran<string>, ExecError<string>>
  export function spawnSync(command: string, args: readonly string[], options: SpawnSyncOptions & Text): Result<Ran<string>, ExecError<string>>
  export function spawnSync(command: string, options?: SpawnSyncOptions & Bytes): Result<Ran<Buffer>, ExecError<Buffer>>
  export function spawnSync(command: string, args?: readonly string[], options?: SpawnSyncOptions & Bytes): Result<Ran<Buffer>, ExecError<Buffer>>
}

declare module "node:fs/promises" {
  import type { Dirent, Stats } from "fs"
  type Path = string | Buffer | URL
  type Data = string | NodeJS.ArrayBufferView
  type Flag =
    | "r" | "rs" | "sr" | "r+" | "rs+" | "sr+"
    | "w" | "wx" | "xw" | "w+" | "wx+" | "xw+"
    | "a" | "ax" | "xa" | "as" | "sa" | "a+" | "ax+" | "xa+" | "as+" | "sa+"
  type Done<T> = Promise<Result<T, SystemError>>
  export function readFile(path: Path, options?: { encoding?: undefined; flag?: Flag; signal?: AbortSignal }): Done<Buffer>
  export function readFile(path: Path, options: BufferEncoding | { encoding: BufferEncoding; flag?: Flag; signal?: AbortSignal }): Done<string>
  export function writeFile(file: Path, data: Data, options?: BufferEncoding | { encoding?: BufferEncoding; flag?: Flag; mode?: number; signal?: AbortSignal }): Done<void>
  export function appendFile(file: Path, data: Data, options?: BufferEncoding | { encoding?: BufferEncoding; flag?: Flag; mode?: number }): Done<void>
  export function access(path: Path, mode?: number): Done<void>
  export function chmod(path: Path, mode: number | string): Done<void>
  export function truncate(path: Path, len?: number): Done<void>
  export function stat(path: Path): Done<Stats>
  export function lstat(path: Path): Done<Stats>
  export function readdir(path: Path, options: { withFileTypes: true; recursive?: boolean }): Done<Dirent[]>
  export function readdir(path: Path, options?: "utf8" | { encoding?: "utf8"; withFileTypes?: false; recursive?: boolean }): Done<string[]>
  export function mkdir(path: Path, options: { recursive: true; mode?: number }): Done<string | undefined>
  export function mkdir(path: Path, options?: number | { recursive?: false; mode?: number }): Done<void>
  export function mkdtemp(prefix: string, options?: "utf8" | { encoding?: "utf8" }): Done<string>
  export function rmdir(path: Path): Done<void>
  export function unlink(path: Path): Done<void>
  export function rm(path: Path, options?: { recursive?: boolean; force?: boolean; maxRetries?: number; retryDelay?: number }): Done<void>
  export function rename(oldPath: Path, newPath: Path): Done<void>
  export function copyFile(src: Path, dest: Path, mode?: number): Done<void>
  export function realpath(path: Path, options?: "utf8" | { encoding?: "utf8" }): Done<string>
}

// Input line by line (docs/io.md, section 4).
declare module "tz:io" {
  // Prints the question as it is and reads a line, without its \n or \r\n;
  // undefined at the end of the input.
  export function readLine(question?: string): string | undefined
}

// A server of HTTP/1.1 and https (docs/net.md, section 1).
declare module "tz:http" {
  // What onListen gets.
  export interface ServeAddress {
    hostname: string
    port: int
  }

  // The certificate of a server of https and its key, PEM.
  export interface ServeTls {
    cert: string
    key: string
  }

  export interface ServeOptions {
    port?: int
    hostname?: string
    tls?: ServeTls
    signal?: AbortSignal
    onListen?: (address: ServeAddress) => void
    // A request with a larger body is answered with 413.
    maxBodySize?: double
  }

  export type Handler = (request: Request) => Response | Promise<Response>

  export class Server {
    // A server without a socket yet; listen opens one.
    constructor(handler: Handler)
    // 0 and "" until it listens.
    readonly port: int
    readonly hostname: string
    // Set once it listens.
    readonly url?: URL
    // Resolved once the server has stopped and its last response is out.
    readonly finished: Promise<void>
    // Opens the socket and starts answering; gives the address. A port that
    // is taken gives an Err; so does a second listen, after stop too
    // (ERR_SERVER_ALREADY_LISTEN): a server listens once.
    listen(options: ServeOptions): Result<URL, SystemError>
    // Stops taking connections and resolves once the requests being handled
    // are answered; before listen it does nothing.
    stop(): Promise<void>
  }

  // Makes a server and listens at once: on `hostname` (0.0.0.0) and `port`
  // (8000; 0 for any free one, which server.port then tells), answering each
  // request with `handler`; of https with `tls`. A port that is taken, a host
  // that is not found or a certificate that cannot be read give an Err.
  export function serve(options: ServeOptions, handler: Handler): Result<Server, SystemError>
}
