/// <reference path="./http.d.ts" />
// What tz has built in, declared for TypeScript's checker in the editor (tsc in
// VS Code): the names a program uses without an import, and the module "tz:io"
// (docs/modules.md, section 6). tz itself does not read this file; tz build
// checks the program by its own rules. "tz:http" is declared in its own
// library/http/http.d.tz, which the build turns into http.d.ts next to this
// file (editors/types/declarations.cmake).
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
type DecimalError = { kind: "DECIMAL_ERROR"; code: "ERR_INVALID_DECIMAL"; message: string; text: string }
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
type NumberError = { kind: "NUMBER_ERROR"; code: "ERR_INVALID_NUMBER"; message: string }
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
type JsonError = {
  kind: "JSON_ERROR"
  code: "ERR_INVALID_JSON" | "ERR_CIRCULAR_STRUCTURE"
  message: string
}
type UriError = { kind: "URI_ERROR"; code: "ERR_INVALID_URI"; message: string }
type RegexError = { kind: "REGEX_ERROR"; code: "ERR_INVALID_REGEX"; message: string }
type UrlError = { kind: "URL_ERROR"; code: "ERR_INVALID_URL"; message: string; input: string }
type DateError = { kind: "DATE_ERROR"; code: "ERR_INVALID_DATE"; message: string; text: string }
type LocaleError = { kind: "LOCALE_ERROR"; code: "ERR_INVALID_LOCALE"; message: string; tag: string }
type DecodeError = { kind: "DECODE_ERROR"; code: "ERR_ENCODING_INVALID_ENCODED_DATA"; message: string }
type EncodingError = { kind: "ENCODING_ERROR"; code: "ERR_ENCODING_NOT_SUPPORTED"; message: string }

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
type TlsCode =
  | "UNABLE_TO_GET_ISSUER_CERT" | "UNABLE_TO_GET_CRL" | "UNABLE_TO_DECRYPT_CERT_SIGNATURE"
  | "UNABLE_TO_DECRYPT_CRL_SIGNATURE" | "UNABLE_TO_DECODE_ISSUER_PUBLIC_KEY"
  | "CERT_SIGNATURE_FAILURE" | "CRL_SIGNATURE_FAILURE" | "CERT_NOT_YET_VALID" | "CERT_HAS_EXPIRED"
  | "CRL_NOT_YET_VALID" | "CRL_HAS_EXPIRED" | "ERROR_IN_CERT_NOT_BEFORE_FIELD"
  | "ERROR_IN_CERT_NOT_AFTER_FIELD" | "ERROR_IN_CRL_LAST_UPDATE_FIELD"
  | "ERROR_IN_CRL_NEXT_UPDATE_FIELD" | "OUT_OF_MEM" | "DEPTH_ZERO_SELF_SIGNED_CERT"
  | "SELF_SIGNED_CERT_IN_CHAIN" | "UNABLE_TO_GET_ISSUER_CERT_LOCALLY"
  | "UNABLE_TO_VERIFY_LEAF_SIGNATURE" | "CERT_CHAIN_TOO_LONG" | "CERT_REVOKED" | "INVALID_CA"
  | "PATH_LENGTH_EXCEEDED" | "INVALID_PURPOSE" | "CERT_UNTRUSTED" | "CERT_REJECTED"
  | "HOSTNAME_MISMATCH" | "UNSPECIFIED" | "ERR_TLS_CERT_ALTNAME_INVALID" | "ERR_TLS_HANDSHAKE"
// What went wrong with a fetch: the kind says what to do (REQUEST — fix the
// code; NETWORK — try again; RESPONSE, TLS, REDIRECT, TIMEOUT), the code is
// one of a closed list (docs/net.md, section 2). E is the error of a stream
// sent as the body: its failure comes whole in details.
type FetchError<E = never> =
  | {
      kind: "FETCH_ERROR_REQUEST"
      code: "ERR_INVALID_URL" | "ERR_CREDENTIALS_IN_URL" | "ERR_INVALID_METHOD" | "ERR_BODY_WITH_GET"
        | "ERR_DUPLEX_REQUIRED" | "ERR_REQUEST_USED" | "ERR_UNSUPPORTED_SCHEME" | "ERR_BLOCKED_PORT"
        | "ERR_BODY_FAILED"
      message: string
      url: string
    }
  | { kind: "FETCH_ERROR_REQUEST"; code: "ERR_STREAM_FAILED"; message: string; url: string; details: E }
  | {
      kind: "FETCH_ERROR_NETWORK"
      code: "ENOTFOUND" | "EAI_AGAIN" | "ECONNREFUSED" | "ETIMEDOUT" | "ENETUNREACH" | "EHOSTUNREACH"
        | "EADDRNOTAVAIL" | "ECONNRESET" | "ECONNABORTED" | "EPIPE" | "EMFILE" | "ENOBUFS" | "UNKNOWN"
      message: string
      syscall: string
      host: string
      port: int
    }
  | {
      kind: "FETCH_ERROR_RESPONSE"
      code: "ERR_CLOSED" | "ERR_INVALID_RESPONSE" | "ERR_HEAD_TOO_LARGE"
      message: string
      host: string
      port: int
    }
  | { kind: "FETCH_ERROR_TLS"; code: TlsCode; message: string; host: string }
  | {
      kind: "FETCH_ERROR_REDIRECT"
      code: "ERR_UNEXPECTED_REDIRECT" | "ERR_TOO_MANY_REDIRECTS" | "ERR_INVALID_LOCATION"
        | "ERR_REDIRECT_WITH_STREAM"
      message: string
      location: string
    }
  | { kind: "FETCH_ERROR_TIMEOUT"; code: "TIMEOUT_ERR"; message: string; timeout: int }
  | {
      kind: "FETCH_ERROR_PROXY"
      code: "ERR_INVALID_PROXY_URL" | "ERR_PROXY_AUTH_REQUIRED" | "ERR_PROXY_REFUSED"
      message: string
      proxy: string
      status: int
    }
// What went wrong while reading a body (docs/net.md, section 3); each is a
// StreamError, as the body is a stream.
interface BodyErrorUsed extends StreamError { kind: "BODY_ERROR_USED"; code: "ERR_BODY_USED" | "ERR_BODY_LOCKED" }
interface BodyErrorNetwork extends StreamError {
  kind: "BODY_ERROR_NETWORK"
  code: "ERR_CLOSED" | "ECONNRESET" | "ECONNABORTED" | "ETIMEDOUT" | "ERR_TLS_FAILED" | "UNKNOWN"
}
interface BodyErrorTimeout extends StreamError { kind: "BODY_ERROR_TIMEOUT"; code: "TIMEOUT_ERR"; timeout: int }
interface BodyErrorTooLarge extends StreamError { kind: "BODY_ERROR_TOO_LARGE"; code: "ERR_BODY_TOO_LARGE"; limit: double }
interface BodyErrorMalformed extends StreamError {
  kind: "BODY_ERROR_MALFORMED"
  code: "ERR_INVALID_CHUNKED" | "ERR_INVALID_COMPRESSION"
}
interface BodyErrorStream extends StreamError { kind: "BODY_ERROR_STREAM"; code: "ERR_STREAM_FAILED" }
type BodyError =
  | BodyErrorUsed
  | BodyErrorNetwork
  | BodyErrorTimeout
  | BodyErrorTooLarge
  | BodyErrorMalformed
  | BodyErrorStream
// A stream read the wrong way: a second reader, or a read after releaseLock().
interface ReaderError extends StreamError {
  kind: "READER_ERROR"
  code: "ERR_STREAM_LOCKED" | "ERR_READER_RELEASED"
}
// A stream its maker broke: enqueue() or close() after close().
interface ControllerClosedError extends StreamError {
  kind: "CONTROLLER_CLOSED_ERROR"
  code: "ERR_INVALID_STATE"
}
// What new Response takes, and what new Request takes: data, never a stream.
type BodyData = string | Uint8Array | ArrayBuffer | URLSearchParams | FormData
interface RequestOptions {
  method?: string
  headers?: HeadersInit
  body?: BodyData
  redirect?: RequestRedirect
  timeout?: int
  duplex?: "half"
}
// The errors of text(), bytes() and arrayBuffer(): those of every body.
type TextError = BodyError
type BytesError = BodyError
// The errors of json(): those of every body, and a body that is not JSON
// (start: its first 200 characters).
type JsonBodyError =
  | BodyError
  | { kind: "JSON_BODY_ERROR_CONTENT"; code: "ERR_INVALID_JSON"; message: string; contentType?: string; start: string }
// The errors of formData(): those of every body, and a body that is not a form.
type FormDataError =
  | BodyError
  | {
      kind: "FORM_DATA_ERROR_CONTENT"
      code: "ERR_NOT_A_FORM" | "ERR_NO_BOUNDARY" | "ERR_INVALID_MULTIPART"
      message: string
      contentType?: string
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

// The parts of a URL are read with methods and changed with methods
// (docs/objects.md, section 6). A value that the URL can refuse gives a
// Result; tz checks a literal when it builds the program.
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
// which crypto.subtle calls.
interface Event {
  immediateStopped(): boolean
  dispatchOn(target: EventTarget, run: () => void): boolean
}

declare module "stream/web" {
  interface ReadableStream<R = any> {
    disturbed(): boolean
  }
}

declare module "crypto" {
  namespace webcrypto {
    interface CryptoKey {
      sign(algorithm: AlgorithmIdentifier, data: BufferSource): Promise<Result<ArrayBuffer, SubtleSignError>>
      verify(algorithm: AlgorithmIdentifier, signature: BufferSource,
             data: BufferSource): Promise<Result<boolean, SubtleVerifyError>>
      encrypt(algorithm: AlgorithmIdentifier, data: BufferSource): Promise<Result<ArrayBuffer, SubtleEncryptError>>
      decrypt(algorithm: AlgorithmIdentifier, data: BufferSource): Promise<Result<ArrayBuffer, SubtleDecryptError>>
      deriveBits(algorithm: AlgorithmIdentifier, length?: number): Promise<Result<ArrayBuffer, SubtleDeriveBitsError>>
      deriveKey(algorithm: AlgorithmIdentifier, derivedKeyAlgorithm: AlgorithmIdentifier,
                extractable: boolean, usages: KeyUsage[]): Promise<Result<CryptoKey, SubtleDeriveKeyError>>
      wrap(format: KeyFormat, key: CryptoKey,
           algorithm: AlgorithmIdentifier): Promise<Result<ArrayBuffer, SubtleWrapKeyError>>
      unwrap(format: KeyFormat, wrappedKey: BufferSource,
             algorithm: AlgorithmIdentifier): Promise<Result<ArrayBuffer | JsonWebKey, SubtleUnwrapKeyError>>
      export(format: KeyFormat): Promise<Result<ArrayBuffer | JsonWebKey, SubtleExportKeyError>>
    }
  }
}

interface RegExpConstructor {
  parse(source: string, flags?: string): Result<RegExp, RegexError>
}

interface JSON {
  // JSON.parse gives a Result with a Json; the program checks what it holds
  // (docs/narrowing.md, section 3).
  parse(text: string): Result<Json, JsonError>
}

// Files built into the executable, read when the program is compiled; the
// path is relative to the file of the call.
declare function embedText(path: string): string
declare function embedBytes(path: string): Buffer

// The names of the web platform that tz has as globals and @types/node only
// inside its modules, or only as values.
interface EventListenerObject {
  handleEvent(event: Event): void
}
type HeadersInit = import("undici-types").HeadersInit
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

// crypto.subtle gives a Result where node rejects (docs/crypto.md, "Ошибки"): a type of
// error for each method, kind = the method and the reason.
type SubtleDigestError =
  | { kind: "SUBTLE_DIGEST_ERROR_UNSUPPORTED_ALGORITHM"; code: "ERR_NOT_SUPPORTED"; message: string }
  | { kind: "SUBTLE_DIGEST_ERROR_WRONG_PARAMETERS"; code: "ERR_OPERATION_FAILED" | "ERR_INVALID_ARG_TYPE"; message: string }
  | { kind: "SUBTLE_DIGEST_ERROR_SYSTEM"; code: "ERR_OPERATION_FAILED"; message: string }
type SubtleGenerateKeyError =
  | { kind: "SUBTLE_GENERATE_KEY_ERROR_UNSUPPORTED_ALGORITHM"; code: "ERR_NOT_SUPPORTED"; message: string }
  | { kind: "SUBTLE_GENERATE_KEY_ERROR_WRONG_PARAMETERS"; code: "ERR_OPERATION_FAILED" | "ERR_INVALID_ARG_TYPE"; message: string }
  | { kind: "SUBTLE_GENERATE_KEY_ERROR_WRONG_USAGE"; code: "ERR_INVALID_KEY_USAGE"; message: string }
  | { kind: "SUBTLE_GENERATE_KEY_ERROR_SYSTEM"; code: "ERR_OPERATION_FAILED"; message: string }
type SubtleImportKeyError =
  | { kind: "SUBTLE_IMPORT_KEY_ERROR_UNSUPPORTED_ALGORITHM"; code: "ERR_NOT_SUPPORTED"; message: string }
  | { kind: "SUBTLE_IMPORT_KEY_ERROR_WRONG_PARAMETERS"; code: "ERR_OPERATION_FAILED" | "ERR_INVALID_ARG_TYPE"; message: string }
  | { kind: "SUBTLE_IMPORT_KEY_ERROR_WRONG_USAGE"; code: "ERR_INVALID_KEY_USAGE"; message: string }
  | { kind: "SUBTLE_IMPORT_KEY_ERROR_NOT_A_KEY"; code: "ERR_INVALID_DATA"; message: string }
  | { kind: "SUBTLE_IMPORT_KEY_ERROR_SYSTEM"; code: "ERR_OPERATION_FAILED"; message: string }
type SubtleExportKeyError =
  | { kind: "SUBTLE_EXPORT_KEY_ERROR_UNSUPPORTED_ALGORITHM"; code: "ERR_NOT_SUPPORTED"; message: string }
  | { kind: "SUBTLE_EXPORT_KEY_ERROR_WRONG_KEY"; code: "ERR_INVALID_ACCESS"; message: string }
  | { kind: "SUBTLE_EXPORT_KEY_ERROR_SYSTEM"; code: "ERR_OPERATION_FAILED"; message: string }
type SubtleSignError =
  | { kind: "SUBTLE_SIGN_ERROR_UNSUPPORTED_ALGORITHM"; code: "ERR_NOT_SUPPORTED"; message: string }
  | { kind: "SUBTLE_SIGN_ERROR_WRONG_PARAMETERS"; code: "ERR_OPERATION_FAILED" | "ERR_INVALID_ARG_TYPE"; message: string }
  | { kind: "SUBTLE_SIGN_ERROR_WRONG_KEY"; code: "ERR_INVALID_ACCESS"; message: string }
  | { kind: "SUBTLE_SIGN_ERROR_SYSTEM"; code: "ERR_OPERATION_FAILED"; message: string }
type SubtleVerifyError =
  | { kind: "SUBTLE_VERIFY_ERROR_UNSUPPORTED_ALGORITHM"; code: "ERR_NOT_SUPPORTED"; message: string }
  | { kind: "SUBTLE_VERIFY_ERROR_WRONG_PARAMETERS"; code: "ERR_OPERATION_FAILED" | "ERR_INVALID_ARG_TYPE"; message: string }
  | { kind: "SUBTLE_VERIFY_ERROR_WRONG_KEY"; code: "ERR_INVALID_ACCESS"; message: string }
  | { kind: "SUBTLE_VERIFY_ERROR_SYSTEM"; code: "ERR_OPERATION_FAILED"; message: string }
type SubtleEncryptError =
  | { kind: "SUBTLE_ENCRYPT_ERROR_UNSUPPORTED_ALGORITHM"; code: "ERR_NOT_SUPPORTED"; message: string }
  | { kind: "SUBTLE_ENCRYPT_ERROR_WRONG_PARAMETERS"; code: "ERR_OPERATION_FAILED" | "ERR_INVALID_ARG_TYPE"; message: string }
  | { kind: "SUBTLE_ENCRYPT_ERROR_WRONG_KEY"; code: "ERR_INVALID_ACCESS"; message: string }
  | { kind: "SUBTLE_ENCRYPT_ERROR_SYSTEM"; code: "ERR_OPERATION_FAILED"; message: string }
type SubtleDecryptError =
  | { kind: "SUBTLE_DECRYPT_ERROR_UNSUPPORTED_ALGORITHM"; code: "ERR_NOT_SUPPORTED"; message: string }
  | { kind: "SUBTLE_DECRYPT_ERROR_WRONG_PARAMETERS"; code: "ERR_OPERATION_FAILED" | "ERR_INVALID_ARG_TYPE"; message: string }
  | { kind: "SUBTLE_DECRYPT_ERROR_WRONG_KEY"; code: "ERR_INVALID_ACCESS"; message: string }
  | { kind: "SUBTLE_DECRYPT_ERROR_DECRYPTION_FAILED"; code: "ERR_OPERATION_FAILED"; message: string }
  | { kind: "SUBTLE_DECRYPT_ERROR_SYSTEM"; code: "ERR_OPERATION_FAILED"; message: string }
type SubtleDeriveBitsError =
  | { kind: "SUBTLE_DERIVE_BITS_ERROR_UNSUPPORTED_ALGORITHM"; code: "ERR_NOT_SUPPORTED"; message: string }
  | { kind: "SUBTLE_DERIVE_BITS_ERROR_WRONG_PARAMETERS"; code: "ERR_OPERATION_FAILED" | "ERR_INVALID_ARG_TYPE"; message: string }
  | { kind: "SUBTLE_DERIVE_BITS_ERROR_WRONG_KEY"; code: "ERR_INVALID_ACCESS"; message: string }
  | { kind: "SUBTLE_DERIVE_BITS_ERROR_SYSTEM"; code: "ERR_OPERATION_FAILED"; message: string }
type SubtleDeriveKeyError =
  | { kind: "SUBTLE_DERIVE_KEY_ERROR_UNSUPPORTED_ALGORITHM"; code: "ERR_NOT_SUPPORTED"; message: string }
  | { kind: "SUBTLE_DERIVE_KEY_ERROR_WRONG_PARAMETERS"; code: "ERR_OPERATION_FAILED" | "ERR_INVALID_ARG_TYPE"; message: string }
  | { kind: "SUBTLE_DERIVE_KEY_ERROR_WRONG_KEY"; code: "ERR_INVALID_ACCESS"; message: string }
  | { kind: "SUBTLE_DERIVE_KEY_ERROR_WRONG_USAGE"; code: "ERR_INVALID_KEY_USAGE"; message: string }
  | { kind: "SUBTLE_DERIVE_KEY_ERROR_NOT_A_KEY"; code: "ERR_INVALID_DATA"; message: string }
  | { kind: "SUBTLE_DERIVE_KEY_ERROR_SYSTEM"; code: "ERR_OPERATION_FAILED"; message: string }
type SubtleWrapKeyError =
  | { kind: "SUBTLE_WRAP_KEY_ERROR_UNSUPPORTED_ALGORITHM"; code: "ERR_NOT_SUPPORTED"; message: string }
  | { kind: "SUBTLE_WRAP_KEY_ERROR_WRONG_PARAMETERS"; code: "ERR_OPERATION_FAILED" | "ERR_INVALID_ARG_TYPE"; message: string }
  | { kind: "SUBTLE_WRAP_KEY_ERROR_WRONG_KEY"; code: "ERR_INVALID_ACCESS"; message: string }
  | { kind: "SUBTLE_WRAP_KEY_ERROR_SYSTEM"; code: "ERR_OPERATION_FAILED"; message: string }
type SubtleUnwrapKeyError =
  | { kind: "SUBTLE_UNWRAP_KEY_ERROR_UNSUPPORTED_ALGORITHM"; code: "ERR_NOT_SUPPORTED"; message: string }
  | { kind: "SUBTLE_UNWRAP_KEY_ERROR_WRONG_PARAMETERS"; code: "ERR_OPERATION_FAILED" | "ERR_INVALID_ARG_TYPE"; message: string }
  | { kind: "SUBTLE_UNWRAP_KEY_ERROR_WRONG_KEY"; code: "ERR_INVALID_ACCESS"; message: string }
  | { kind: "SUBTLE_UNWRAP_KEY_ERROR_WRONG_USAGE"; code: "ERR_INVALID_KEY_USAGE"; message: string }
  | { kind: "SUBTLE_UNWRAP_KEY_ERROR_NOT_A_KEY"; code: "ERR_INVALID_DATA"; message: string }
  | { kind: "SUBTLE_UNWRAP_KEY_ERROR_DECRYPTION_FAILED"; code: "ERR_OPERATION_FAILED"; message: string }
  | { kind: "SUBTLE_UNWRAP_KEY_ERROR_SYSTEM"; code: "ERR_OPERATION_FAILED"; message: string }

// Why rl.question() of node:readline/promises gave an Err: the Interface is
// closed.
type QuestionError = { kind: "QUESTION_ERROR"; code: "ERR_USE_AFTER_CLOSE"; message: string }

// The error of any stream: each has a message (docs/streams.md, section 3).
// A stream of its own errors declares them as 'interface X extends StreamError'.
interface StreamError {
  message: string
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
  export type ReadFileError =
    | { kind: "READ_FILE_ERROR_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; path: string }
    | { kind: "READ_FILE_ERROR_WRONG_TYPE"; code: "EISDIR" | "ENOTDIR"; message: string; syscall: string; path: string }
    | { kind: "READ_FILE_ERROR_PERMISSION"; code: "EACCES" | "EPERM"; message: string; syscall: string; path: string }
    | { kind: "READ_FILE_ERROR_BUSY"; code: "EBUSY" | "EAGAIN" | "EMFILE" | "ENFILE"; message: string; syscall: string; path: string }
    | { kind: "READ_FILE_ERROR_PATH"; code: "ERR_INVALID_PATH" | "ENAMETOOLONG" | "ELOOP" | "EINVAL"; message: string; syscall: string; path: string }
    | { kind: "READ_FILE_ERROR_SYSTEM"; code: "EIO" | "ENOMEM" | "ETIMEDOUT" | "EBADF" | "UNKNOWN"; message: string; syscall: string; path: string }
  export type WriteFileError =
    | { kind: "WRITE_FILE_ERROR_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; path: string }
    | { kind: "WRITE_FILE_ERROR_WRONG_TYPE"; code: "EISDIR" | "ENOTDIR"; message: string; syscall: string; path: string }
    | { kind: "WRITE_FILE_ERROR_PERMISSION"; code: "EACCES" | "EPERM" | "EROFS"; message: string; syscall: string; path: string }
    | { kind: "WRITE_FILE_ERROR_BUSY"; code: "EBUSY" | "ETXTBSY" | "EAGAIN" | "EMFILE" | "ENFILE"; message: string; syscall: string; path: string }
    | { kind: "WRITE_FILE_ERROR_FULL"; code: "ENOSPC" | "EFBIG"; message: string; syscall: string; path: string }
    | { kind: "WRITE_FILE_ERROR_PATH"; code: "ERR_INVALID_PATH" | "ENAMETOOLONG" | "ELOOP" | "EINVAL"; message: string; syscall: string; path: string }
    | { kind: "WRITE_FILE_ERROR_SYSTEM"; code: "EIO" | "ENOMEM" | "ETIMEDOUT" | "EBADF" | "EPIPE" | "EOF" | "UNKNOWN"; message: string; syscall: string; path: string }
  export type WriteNewFileError =
    | { kind: "WRITE_NEW_FILE_ERROR_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; path: string }
    | { kind: "WRITE_NEW_FILE_ERROR_EXISTS"; code: "EEXIST"; message: string; syscall: string; path: string }
    | { kind: "WRITE_NEW_FILE_ERROR_WRONG_TYPE"; code: "EISDIR" | "ENOTDIR"; message: string; syscall: string; path: string }
    | { kind: "WRITE_NEW_FILE_ERROR_PERMISSION"; code: "EACCES" | "EPERM" | "EROFS"; message: string; syscall: string; path: string }
    | { kind: "WRITE_NEW_FILE_ERROR_BUSY"; code: "EBUSY" | "ETXTBSY" | "EAGAIN" | "EMFILE" | "ENFILE"; message: string; syscall: string; path: string }
    | { kind: "WRITE_NEW_FILE_ERROR_FULL"; code: "ENOSPC" | "EFBIG"; message: string; syscall: string; path: string }
    | { kind: "WRITE_NEW_FILE_ERROR_PATH"; code: "ERR_INVALID_PATH" | "ENAMETOOLONG" | "ELOOP" | "EINVAL"; message: string; syscall: string; path: string }
    | { kind: "WRITE_NEW_FILE_ERROR_SYSTEM"; code: "EIO" | "ENOMEM" | "ETIMEDOUT" | "EBADF" | "EPIPE" | "EOF" | "UNKNOWN"; message: string; syscall: string; path: string }
  export type AppendFileError =
    | { kind: "APPEND_FILE_ERROR_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; path: string }
    | { kind: "APPEND_FILE_ERROR_WRONG_TYPE"; code: "EISDIR" | "ENOTDIR"; message: string; syscall: string; path: string }
    | { kind: "APPEND_FILE_ERROR_PERMISSION"; code: "EACCES" | "EPERM" | "EROFS"; message: string; syscall: string; path: string }
    | { kind: "APPEND_FILE_ERROR_BUSY"; code: "EBUSY" | "ETXTBSY" | "EAGAIN" | "EMFILE" | "ENFILE"; message: string; syscall: string; path: string }
    | { kind: "APPEND_FILE_ERROR_FULL"; code: "ENOSPC" | "EFBIG"; message: string; syscall: string; path: string }
    | { kind: "APPEND_FILE_ERROR_PATH"; code: "ERR_INVALID_PATH" | "ENAMETOOLONG" | "ELOOP" | "EINVAL"; message: string; syscall: string; path: string }
    | { kind: "APPEND_FILE_ERROR_SYSTEM"; code: "EIO" | "ENOMEM" | "ETIMEDOUT" | "EBADF" | "EPIPE" | "EOF" | "UNKNOWN"; message: string; syscall: string; path: string }
  export type AppendNewFileError =
    | { kind: "APPEND_NEW_FILE_ERROR_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; path: string }
    | { kind: "APPEND_NEW_FILE_ERROR_EXISTS"; code: "EEXIST"; message: string; syscall: string; path: string }
    | { kind: "APPEND_NEW_FILE_ERROR_WRONG_TYPE"; code: "EISDIR" | "ENOTDIR"; message: string; syscall: string; path: string }
    | { kind: "APPEND_NEW_FILE_ERROR_PERMISSION"; code: "EACCES" | "EPERM" | "EROFS"; message: string; syscall: string; path: string }
    | { kind: "APPEND_NEW_FILE_ERROR_BUSY"; code: "EBUSY" | "ETXTBSY" | "EAGAIN" | "EMFILE" | "ENFILE"; message: string; syscall: string; path: string }
    | { kind: "APPEND_NEW_FILE_ERROR_FULL"; code: "ENOSPC" | "EFBIG"; message: string; syscall: string; path: string }
    | { kind: "APPEND_NEW_FILE_ERROR_PATH"; code: "ERR_INVALID_PATH" | "ENAMETOOLONG" | "ELOOP" | "EINVAL"; message: string; syscall: string; path: string }
    | { kind: "APPEND_NEW_FILE_ERROR_SYSTEM"; code: "EIO" | "ENOMEM" | "ETIMEDOUT" | "EBADF" | "EPIPE" | "EOF" | "UNKNOWN"; message: string; syscall: string; path: string }
  export type TruncateError =
    | { kind: "TRUNCATE_ERROR_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; path: string }
    | { kind: "TRUNCATE_ERROR_WRONG_TYPE"; code: "EISDIR" | "ENOTDIR"; message: string; syscall: string; path: string }
    | { kind: "TRUNCATE_ERROR_PERMISSION"; code: "EACCES" | "EPERM" | "EROFS"; message: string; syscall: string; path: string }
    | { kind: "TRUNCATE_ERROR_BUSY"; code: "EBUSY" | "ETXTBSY" | "EMFILE" | "ENFILE"; message: string; syscall: string; path: string }
    | { kind: "TRUNCATE_ERROR_FULL"; code: "ENOSPC" | "EFBIG"; message: string; syscall: string; path: string }
    | { kind: "TRUNCATE_ERROR_PATH"; code: "ERR_INVALID_PATH" | "ENAMETOOLONG" | "ELOOP" | "EINVAL"; message: string; syscall: string; path: string }
    | { kind: "TRUNCATE_ERROR_SYSTEM"; code: "EIO" | "ENOMEM" | "ETIMEDOUT" | "UNKNOWN"; message: string; syscall: string; path: string }
  export type AccessError =
    | { kind: "ACCESS_ERROR_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; path: string }
    | { kind: "ACCESS_ERROR_WRONG_TYPE"; code: "ENOTDIR"; message: string; syscall: string; path: string }
    | { kind: "ACCESS_ERROR_PERMISSION"; code: "EACCES" | "EPERM" | "EROFS"; message: string; syscall: string; path: string }
    | { kind: "ACCESS_ERROR_PATH"; code: "ERR_INVALID_PATH" | "ENAMETOOLONG" | "ELOOP" | "EINVAL"; message: string; syscall: string; path: string }
    | { kind: "ACCESS_ERROR_SYSTEM"; code: "EIO" | "ENOMEM" | "ETIMEDOUT" | "UNKNOWN"; message: string; syscall: string; path: string }
  export type ChangeModeError =
    | { kind: "CHANGE_MODE_ERROR_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; path: string }
    | { kind: "CHANGE_MODE_ERROR_WRONG_TYPE"; code: "ENOTDIR"; message: string; syscall: string; path: string }
    | { kind: "CHANGE_MODE_ERROR_PERMISSION"; code: "EACCES" | "EPERM" | "EROFS"; message: string; syscall: string; path: string }
    | { kind: "CHANGE_MODE_ERROR_PATH"; code: "ERR_INVALID_PATH" | "ENAMETOOLONG" | "ELOOP" | "EINVAL"; message: string; syscall: string; path: string }
    | { kind: "CHANGE_MODE_ERROR_SYSTEM"; code: "EIO" | "ENOMEM" | "ETIMEDOUT" | "UNKNOWN"; message: string; syscall: string; path: string }
  export type StatusError =
    | { kind: "STATUS_ERROR_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; path: string }
    | { kind: "STATUS_ERROR_WRONG_TYPE"; code: "ENOTDIR"; message: string; syscall: string; path: string }
    | { kind: "STATUS_ERROR_PERMISSION"; code: "EACCES" | "EPERM"; message: string; syscall: string; path: string }
    | { kind: "STATUS_ERROR_BUSY"; code: "EBUSY"; message: string; syscall: string; path: string }
    | { kind: "STATUS_ERROR_PATH"; code: "ERR_INVALID_PATH" | "ENAMETOOLONG" | "ELOOP" | "EINVAL"; message: string; syscall: string; path: string }
    | { kind: "STATUS_ERROR_SYSTEM"; code: "EIO" | "ENOMEM" | "ETIMEDOUT" | "UNKNOWN"; message: string; syscall: string; path: string }
  export type StatusIfExistsError =
    | { kind: "STATUS_IF_EXISTS_ERROR_WRONG_TYPE"; code: "ENOTDIR"; message: string; syscall: string; path: string }
    | { kind: "STATUS_IF_EXISTS_ERROR_PERMISSION"; code: "EACCES" | "EPERM"; message: string; syscall: string; path: string }
    | { kind: "STATUS_IF_EXISTS_ERROR_BUSY"; code: "EBUSY"; message: string; syscall: string; path: string }
    | { kind: "STATUS_IF_EXISTS_ERROR_PATH"; code: "ERR_INVALID_PATH" | "ENAMETOOLONG" | "ELOOP" | "EINVAL"; message: string; syscall: string; path: string }
    | { kind: "STATUS_IF_EXISTS_ERROR_SYSTEM"; code: "EIO" | "ENOMEM" | "ETIMEDOUT" | "UNKNOWN"; message: string; syscall: string; path: string }
  export type LinkStatusError =
    | { kind: "LINK_STATUS_ERROR_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; path: string }
    | { kind: "LINK_STATUS_ERROR_WRONG_TYPE"; code: "ENOTDIR"; message: string; syscall: string; path: string }
    | { kind: "LINK_STATUS_ERROR_PERMISSION"; code: "EACCES" | "EPERM"; message: string; syscall: string; path: string }
    | { kind: "LINK_STATUS_ERROR_BUSY"; code: "EBUSY"; message: string; syscall: string; path: string }
    | { kind: "LINK_STATUS_ERROR_PATH"; code: "ERR_INVALID_PATH" | "ENAMETOOLONG" | "ELOOP" | "EINVAL"; message: string; syscall: string; path: string }
    | { kind: "LINK_STATUS_ERROR_SYSTEM"; code: "EIO" | "ENOMEM" | "ETIMEDOUT" | "UNKNOWN"; message: string; syscall: string; path: string }
  export type LinkStatusIfExistsError =
    | { kind: "LINK_STATUS_IF_EXISTS_ERROR_WRONG_TYPE"; code: "ENOTDIR"; message: string; syscall: string; path: string }
    | { kind: "LINK_STATUS_IF_EXISTS_ERROR_PERMISSION"; code: "EACCES" | "EPERM"; message: string; syscall: string; path: string }
    | { kind: "LINK_STATUS_IF_EXISTS_ERROR_BUSY"; code: "EBUSY"; message: string; syscall: string; path: string }
    | { kind: "LINK_STATUS_IF_EXISTS_ERROR_PATH"; code: "ERR_INVALID_PATH" | "ENAMETOOLONG" | "ELOOP" | "EINVAL"; message: string; syscall: string; path: string }
    | { kind: "LINK_STATUS_IF_EXISTS_ERROR_SYSTEM"; code: "EIO" | "ENOMEM" | "ETIMEDOUT" | "UNKNOWN"; message: string; syscall: string; path: string }
  export type ReadDirectoryError =
    | { kind: "READ_DIRECTORY_ERROR_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; path: string }
    | { kind: "READ_DIRECTORY_ERROR_WRONG_TYPE"; code: "ENOTDIR"; message: string; syscall: string; path: string }
    | { kind: "READ_DIRECTORY_ERROR_PERMISSION"; code: "EACCES" | "EPERM"; message: string; syscall: string; path: string }
    | { kind: "READ_DIRECTORY_ERROR_BUSY"; code: "EMFILE" | "ENFILE"; message: string; syscall: string; path: string }
    | { kind: "READ_DIRECTORY_ERROR_PATH"; code: "ERR_INVALID_PATH" | "ENAMETOOLONG" | "ELOOP" | "EINVAL" | "ECHARSET"; message: string; syscall: string; path: string }
    | { kind: "READ_DIRECTORY_ERROR_SYSTEM"; code: "EIO" | "ENOMEM" | "ETIMEDOUT" | "UNKNOWN"; message: string; syscall: string; path: string }
  export type RealPathError =
    | { kind: "REAL_PATH_ERROR_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; path: string }
    | { kind: "REAL_PATH_ERROR_WRONG_TYPE"; code: "ENOTDIR"; message: string; syscall: string; path: string }
    | { kind: "REAL_PATH_ERROR_PERMISSION"; code: "EACCES" | "EPERM"; message: string; syscall: string; path: string }
    | { kind: "REAL_PATH_ERROR_BUSY"; code: "EBUSY"; message: string; syscall: string; path: string }
    | { kind: "REAL_PATH_ERROR_PATH"; code: "ERR_INVALID_PATH" | "ENAMETOOLONG" | "ELOOP" | "EINVAL" | "ECHARSET"; message: string; syscall: string; path: string }
    | { kind: "REAL_PATH_ERROR_SYSTEM"; code: "EIO" | "ENOMEM" | "ETIMEDOUT" | "UNKNOWN"; message: string; syscall: string; path: string }
  export type MakeDirectoryError =
    | { kind: "MAKE_DIRECTORY_ERROR_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; path: string }
    | { kind: "MAKE_DIRECTORY_ERROR_EXISTS"; code: "EEXIST"; message: string; syscall: string; path: string }
    | { kind: "MAKE_DIRECTORY_ERROR_WRONG_TYPE"; code: "ENOTDIR"; message: string; syscall: string; path: string }
    | { kind: "MAKE_DIRECTORY_ERROR_PERMISSION"; code: "EACCES" | "EPERM" | "EROFS"; message: string; syscall: string; path: string }
    | { kind: "MAKE_DIRECTORY_ERROR_FULL"; code: "ENOSPC" | "EMLINK"; message: string; syscall: string; path: string }
    | { kind: "MAKE_DIRECTORY_ERROR_PATH"; code: "ERR_INVALID_PATH" | "ENAMETOOLONG" | "ELOOP" | "EINVAL"; message: string; syscall: string; path: string }
    | { kind: "MAKE_DIRECTORY_ERROR_SYSTEM"; code: "EIO" | "ENOMEM" | "ETIMEDOUT" | "UNKNOWN"; message: string; syscall: string; path: string }
  export type MakeTemporaryDirectoryError =
    | { kind: "MAKE_TEMPORARY_DIRECTORY_ERROR_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; path: string }
    | { kind: "MAKE_TEMPORARY_DIRECTORY_ERROR_WRONG_TYPE"; code: "ENOTDIR"; message: string; syscall: string; path: string }
    | { kind: "MAKE_TEMPORARY_DIRECTORY_ERROR_PERMISSION"; code: "EACCES" | "EPERM" | "EROFS"; message: string; syscall: string; path: string }
    | { kind: "MAKE_TEMPORARY_DIRECTORY_ERROR_FULL"; code: "ENOSPC" | "EMLINK"; message: string; syscall: string; path: string }
    | { kind: "MAKE_TEMPORARY_DIRECTORY_ERROR_PATH"; code: "ERR_INVALID_PATH" | "ENAMETOOLONG" | "ELOOP" | "EINVAL"; message: string; syscall: string; path: string }
    | { kind: "MAKE_TEMPORARY_DIRECTORY_ERROR_SYSTEM"; code: "EIO" | "ENOMEM" | "ETIMEDOUT" | "UNKNOWN"; message: string; syscall: string; path: string }
  export type RemoveDirectoryError =
    | { kind: "REMOVE_DIRECTORY_ERROR_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; path: string }
    | { kind: "REMOVE_DIRECTORY_ERROR_NOT_EMPTY"; code: "ENOTEMPTY"; message: string; syscall: string; path: string }
    | { kind: "REMOVE_DIRECTORY_ERROR_WRONG_TYPE"; code: "ENOTDIR"; message: string; syscall: string; path: string }
    | { kind: "REMOVE_DIRECTORY_ERROR_PERMISSION"; code: "EACCES" | "EPERM" | "EROFS"; message: string; syscall: string; path: string }
    | { kind: "REMOVE_DIRECTORY_ERROR_BUSY"; code: "EBUSY"; message: string; syscall: string; path: string }
    | { kind: "REMOVE_DIRECTORY_ERROR_PATH"; code: "ERR_INVALID_PATH" | "ENAMETOOLONG" | "ELOOP" | "EINVAL"; message: string; syscall: string; path: string }
    | { kind: "REMOVE_DIRECTORY_ERROR_SYSTEM"; code: "EIO" | "ENOMEM" | "ETIMEDOUT" | "UNKNOWN"; message: string; syscall: string; path: string }
  export type UnlinkError =
    | { kind: "UNLINK_ERROR_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; path: string }
    | { kind: "UNLINK_ERROR_WRONG_TYPE"; code: "EISDIR" | "ENOTDIR"; message: string; syscall: string; path: string }
    | { kind: "UNLINK_ERROR_PERMISSION"; code: "EACCES" | "EPERM" | "EROFS"; message: string; syscall: string; path: string }
    | { kind: "UNLINK_ERROR_BUSY"; code: "EBUSY"; message: string; syscall: string; path: string }
    | { kind: "UNLINK_ERROR_PATH"; code: "ERR_INVALID_PATH" | "ENAMETOOLONG" | "ELOOP" | "EINVAL"; message: string; syscall: string; path: string }
    | { kind: "UNLINK_ERROR_SYSTEM"; code: "EIO" | "ENOMEM" | "ETIMEDOUT" | "UNKNOWN"; message: string; syscall: string; path: string }
  export type RemoveError =
    | { kind: "REMOVE_ERROR_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; path: string }
    | { kind: "REMOVE_ERROR_NOT_EMPTY"; code: "ENOTEMPTY"; message: string; syscall: string; path: string }
    | { kind: "REMOVE_ERROR_WRONG_TYPE"; code: "EISDIR" | "ENOTDIR"; message: string; syscall: string; path: string }
    | { kind: "REMOVE_ERROR_PERMISSION"; code: "EACCES" | "EPERM" | "EROFS"; message: string; syscall: string; path: string }
    | { kind: "REMOVE_ERROR_BUSY"; code: "EBUSY" | "EMFILE" | "ENFILE"; message: string; syscall: string; path: string }
    | { kind: "REMOVE_ERROR_PATH"; code: "ERR_INVALID_PATH" | "ENAMETOOLONG" | "ELOOP" | "EINVAL" | "ECHARSET"; message: string; syscall: string; path: string }
    | { kind: "REMOVE_ERROR_SYSTEM"; code: "EIO" | "ENOMEM" | "ETIMEDOUT" | "UNKNOWN"; message: string; syscall: string; path: string }
  export type ForceRemoveError =
    | { kind: "FORCE_REMOVE_ERROR_NOT_EMPTY"; code: "ENOTEMPTY"; message: string; syscall: string; path: string }
    | { kind: "FORCE_REMOVE_ERROR_WRONG_TYPE"; code: "EISDIR" | "ENOTDIR"; message: string; syscall: string; path: string }
    | { kind: "FORCE_REMOVE_ERROR_PERMISSION"; code: "EACCES" | "EPERM" | "EROFS"; message: string; syscall: string; path: string }
    | { kind: "FORCE_REMOVE_ERROR_BUSY"; code: "EBUSY" | "EMFILE" | "ENFILE"; message: string; syscall: string; path: string }
    | { kind: "FORCE_REMOVE_ERROR_PATH"; code: "ERR_INVALID_PATH" | "ENAMETOOLONG" | "ELOOP" | "EINVAL" | "ECHARSET"; message: string; syscall: string; path: string }
    | { kind: "FORCE_REMOVE_ERROR_SYSTEM"; code: "EIO" | "ENOMEM" | "ETIMEDOUT" | "UNKNOWN"; message: string; syscall: string; path: string }
  export type RenameError =
    | { kind: "RENAME_ERROR_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; path: string; dest: string }
    | { kind: "RENAME_ERROR_EXISTS"; code: "EEXIST"; message: string; syscall: string; path: string; dest: string }
    | { kind: "RENAME_ERROR_WRONG_TYPE"; code: "ENOTDIR"; message: string; syscall: string; path: string; dest: string }
    | { kind: "RENAME_ERROR_PERMISSION"; code: "EACCES" | "EPERM" | "EROFS"; message: string; syscall: string; path: string; dest: string }
    | { kind: "RENAME_ERROR_BUSY"; code: "EBUSY"; message: string; syscall: string; path: string; dest: string }
    | { kind: "RENAME_ERROR_FULL"; code: "ENOSPC" | "EMLINK"; message: string; syscall: string; path: string; dest: string }
    | { kind: "RENAME_ERROR_PATH"; code: "ERR_INVALID_PATH" | "ENAMETOOLONG" | "ELOOP" | "EINVAL"; message: string; syscall: string; path: string; dest: string }
    | { kind: "RENAME_ERROR_UNSUPPORTED"; code: "EXDEV"; message: string; syscall: string; path: string; dest: string }
    | { kind: "RENAME_ERROR_SYSTEM"; code: "EIO" | "ENOMEM" | "ETIMEDOUT" | "UNKNOWN"; message: string; syscall: string; path: string; dest: string }
  export type CopyFileError =
    | { kind: "COPY_FILE_ERROR_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; path: string; dest: string }
    | { kind: "COPY_FILE_ERROR_EXISTS"; code: "EEXIST"; message: string; syscall: string; path: string; dest: string }
    | { kind: "COPY_FILE_ERROR_WRONG_TYPE"; code: "EISDIR" | "ENOTDIR"; message: string; syscall: string; path: string; dest: string }
    | { kind: "COPY_FILE_ERROR_PERMISSION"; code: "EACCES" | "EPERM" | "EROFS"; message: string; syscall: string; path: string; dest: string }
    | { kind: "COPY_FILE_ERROR_BUSY"; code: "EBUSY" | "ETXTBSY" | "EMFILE" | "ENFILE"; message: string; syscall: string; path: string; dest: string }
    | { kind: "COPY_FILE_ERROR_FULL"; code: "ENOSPC" | "EFBIG"; message: string; syscall: string; path: string; dest: string }
    | { kind: "COPY_FILE_ERROR_PATH"; code: "ERR_INVALID_PATH" | "ENAMETOOLONG" | "ELOOP" | "EINVAL"; message: string; syscall: string; path: string; dest: string }
    | { kind: "COPY_FILE_ERROR_SYSTEM"; code: "EIO" | "ENOMEM" | "ETIMEDOUT" | "UNKNOWN"; message: string; syscall: string; path: string; dest: string }
  export interface ReadStreamErrorNotFound extends StreamError { kind: "READ_STREAM_ERROR_NOT_FOUND"; code: "ENOENT"; syscall: string; path: string }
  export interface ReadStreamErrorWrongType extends StreamError { kind: "READ_STREAM_ERROR_WRONG_TYPE"; code: "EISDIR" | "ENOTDIR"; syscall: string; path: string }
  export interface ReadStreamErrorPermission extends StreamError { kind: "READ_STREAM_ERROR_PERMISSION"; code: "EACCES" | "EPERM"; syscall: string; path: string }
  export interface ReadStreamErrorBusy extends StreamError { kind: "READ_STREAM_ERROR_BUSY"; code: "EBUSY" | "EAGAIN" | "EMFILE" | "ENFILE"; syscall: string; path: string }
  export interface ReadStreamErrorPath extends StreamError { kind: "READ_STREAM_ERROR_PATH"; code: "ERR_INVALID_PATH" | "ENAMETOOLONG" | "ELOOP" | "EINVAL"; syscall: string; path: string }
  export interface ReadStreamErrorSystem extends StreamError { kind: "READ_STREAM_ERROR_SYSTEM"; code: "EIO" | "ENOMEM" | "ETIMEDOUT" | "EBADF" | "UNKNOWN"; syscall: string; path: string }
  export interface ReadStreamErrorInvalidRange extends StreamError { kind: "READ_STREAM_ERROR_INVALID_RANGE"; code: "ERR_OUT_OF_RANGE"; path: string }
  export type ReadStreamError = ReadStreamErrorNotFound | ReadStreamErrorWrongType | ReadStreamErrorPermission | ReadStreamErrorBusy | ReadStreamErrorPath | ReadStreamErrorSystem | ReadStreamErrorInvalidRange
  export interface WriteStreamErrorNotFound extends StreamError { kind: "WRITE_STREAM_ERROR_NOT_FOUND"; code: "ENOENT"; syscall: string; path: string }
  export interface WriteStreamErrorWrongType extends StreamError { kind: "WRITE_STREAM_ERROR_WRONG_TYPE"; code: "EISDIR" | "ENOTDIR"; syscall: string; path: string }
  export interface WriteStreamErrorPermission extends StreamError { kind: "WRITE_STREAM_ERROR_PERMISSION"; code: "EACCES" | "EPERM" | "EROFS"; syscall: string; path: string }
  export interface WriteStreamErrorBusy extends StreamError { kind: "WRITE_STREAM_ERROR_BUSY"; code: "EBUSY" | "ETXTBSY" | "EAGAIN" | "EMFILE" | "ENFILE"; syscall: string; path: string }
  export interface WriteStreamErrorFull extends StreamError { kind: "WRITE_STREAM_ERROR_FULL"; code: "ENOSPC" | "EFBIG"; syscall: string; path: string }
  export interface WriteStreamErrorPath extends StreamError { kind: "WRITE_STREAM_ERROR_PATH"; code: "ERR_INVALID_PATH" | "ENAMETOOLONG" | "ELOOP" | "EINVAL"; syscall: string; path: string }
  export interface WriteStreamErrorSystem extends StreamError { kind: "WRITE_STREAM_ERROR_SYSTEM"; code: "EIO" | "ENOMEM" | "ETIMEDOUT" | "EBADF" | "EPIPE" | "EOF" | "UNKNOWN"; syscall: string; path: string }
  export interface WriteStreamErrorInvalidRange extends StreamError { kind: "WRITE_STREAM_ERROR_INVALID_RANGE"; code: "ERR_OUT_OF_RANGE"; path: string }
  export type WriteStreamError = WriteStreamErrorNotFound | WriteStreamErrorWrongType | WriteStreamErrorPermission | WriteStreamErrorBusy | WriteStreamErrorFull | WriteStreamErrorPath | WriteStreamErrorSystem | WriteStreamErrorInvalidRange
  export interface WriteNewStreamErrorNotFound extends StreamError { kind: "WRITE_NEW_STREAM_ERROR_NOT_FOUND"; code: "ENOENT"; syscall: string; path: string }
  export interface WriteNewStreamErrorExists extends StreamError { kind: "WRITE_NEW_STREAM_ERROR_EXISTS"; code: "EEXIST"; syscall: string; path: string }
  export interface WriteNewStreamErrorWrongType extends StreamError { kind: "WRITE_NEW_STREAM_ERROR_WRONG_TYPE"; code: "EISDIR" | "ENOTDIR"; syscall: string; path: string }
  export interface WriteNewStreamErrorPermission extends StreamError { kind: "WRITE_NEW_STREAM_ERROR_PERMISSION"; code: "EACCES" | "EPERM" | "EROFS"; syscall: string; path: string }
  export interface WriteNewStreamErrorBusy extends StreamError { kind: "WRITE_NEW_STREAM_ERROR_BUSY"; code: "EBUSY" | "ETXTBSY" | "EAGAIN" | "EMFILE" | "ENFILE"; syscall: string; path: string }
  export interface WriteNewStreamErrorFull extends StreamError { kind: "WRITE_NEW_STREAM_ERROR_FULL"; code: "ENOSPC" | "EFBIG"; syscall: string; path: string }
  export interface WriteNewStreamErrorPath extends StreamError { kind: "WRITE_NEW_STREAM_ERROR_PATH"; code: "ERR_INVALID_PATH" | "ENAMETOOLONG" | "ELOOP" | "EINVAL"; syscall: string; path: string }
  export interface WriteNewStreamErrorSystem extends StreamError { kind: "WRITE_NEW_STREAM_ERROR_SYSTEM"; code: "EIO" | "ENOMEM" | "ETIMEDOUT" | "EBADF" | "EPIPE" | "EOF" | "UNKNOWN"; syscall: string; path: string }
  export interface WriteNewStreamErrorInvalidRange extends StreamError { kind: "WRITE_NEW_STREAM_ERROR_INVALID_RANGE"; code: "ERR_OUT_OF_RANGE"; path: string }
  export type WriteNewStreamError = WriteNewStreamErrorNotFound | WriteNewStreamErrorExists | WriteNewStreamErrorWrongType | WriteNewStreamErrorPermission | WriteNewStreamErrorBusy | WriteNewStreamErrorFull | WriteNewStreamErrorPath | WriteNewStreamErrorSystem | WriteNewStreamErrorInvalidRange
  // The stream of createWriteStream(path, { flags: "wx" }): a new file, whose errors have EXISTS.
  export class WriteNewStream extends WriteStream {}
  type Done<T, E> = Result<T, E>
  type NewFlag = "wx" | "xw" | "wx+" | "xw+"
  type AppendFlag = "a" | "as" | "sa" | "a+" | "as+" | "sa+"
  type AppendNewFlag = "ax" | "xa" | "ax+" | "xa+"
  export function readFileSync(path: File, options?: { encoding?: undefined; flag?: Flag }): Done<Buffer, ReadFileError>
  export function readFileSync(path: File, options: BufferEncoding | { encoding: BufferEncoding; flag?: Flag }): Done<string, ReadFileError>
  export function writeFileSync(file: File, data: Data, options: { encoding?: BufferEncoding; flag: NewFlag; mode?: number }): Done<void, WriteNewFileError>
  export function writeFileSync(file: File, data: Data, options: { encoding?: BufferEncoding; flag: AppendFlag; mode?: number }): Done<void, AppendFileError>
  export function writeFileSync(file: File, data: Data, options: { encoding?: BufferEncoding; flag: AppendNewFlag; mode?: number }): Done<void, AppendNewFileError>
  export function writeFileSync(file: File, data: Data, options?: BufferEncoding | { encoding?: BufferEncoding; flag?: Flag; mode?: number }): Done<void, WriteFileError>
  export function appendFileSync(file: File, data: Data, options: { encoding?: BufferEncoding; flag: AppendNewFlag; mode?: number }): Done<void, AppendNewFileError>
  export function appendFileSync(file: File, data: Data, options?: BufferEncoding | { encoding?: BufferEncoding; flag?: Flag; mode?: number }): Done<void, AppendFileError>
  export function accessSync(path: Path, mode?: number): Done<void, AccessError>
  export function chmodSync(path: Path, mode: number | string): Done<void, ChangeModeError>
  export function truncateSync(path: Path, len?: number): Done<void, TruncateError>
  export function statSync(path: Path, options: { throwIfNoEntry: false }): Done<Stats | undefined, StatusIfExistsError>
  export function statSync(path: Path, options?: { throwIfNoEntry?: true }): Done<Stats, StatusError>
  export function lstatSync(path: Path, options: { throwIfNoEntry: false }): Done<Stats | undefined, LinkStatusIfExistsError>
  export function lstatSync(path: Path, options?: { throwIfNoEntry?: true }): Done<Stats, LinkStatusError>
  export function readdirSync(path: Path, options: { withFileTypes: true; recursive?: boolean }): Done<Dirent[], ReadDirectoryError>
  export function readdirSync(path: Path, options?: "utf8" | { encoding?: "utf8"; withFileTypes?: false; recursive?: boolean }): Done<string[], ReadDirectoryError>
  export function mkdirSync(path: Path, options: { recursive: true; mode?: number }): Done<string | undefined, MakeDirectoryError>
  export function mkdirSync(path: Path, options?: number | { recursive?: false; mode?: number }): Done<void, MakeDirectoryError>
  export function mkdtempSync(prefix: string, options?: "utf8" | { encoding?: "utf8" }): Done<string, MakeTemporaryDirectoryError>
  export function rmdirSync(path: Path): Done<void, RemoveDirectoryError>
  export function unlinkSync(path: Path): Done<void, UnlinkError>
  export function rmSync(path: Path, options: { recursive?: boolean; force: true; maxRetries?: number; retryDelay?: number }): Done<void, ForceRemoveError>
  export function rmSync(path: Path, options?: { recursive?: boolean; force?: boolean; maxRetries?: number; retryDelay?: number }): Done<void, RemoveError>
  export function renameSync(oldPath: Path, newPath: Path): Done<void, RenameError>
  export function copyFileSync(src: Path, dest: Path): Done<void, CopyFileError>
  export function realpathSync(path: Path, options?: "utf8" | { encoding?: "utf8" }): Done<string, RealPathError>
  export const promises: typeof import("node:fs/promises")
}

declare module "node:child_process" {
  import type { ExecFileSyncOptions, ExecSyncOptions, SpawnSyncOptions } from "child_process"
  type Text = { encoding: BufferEncoding }
  type Bytes = { encoding?: "buffer" }
  type Shell = { shell: true | string }
  export type ExecTextError =
    | { kind: "EXEC_TEXT_ERROR_SHELL_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; command: string; shell: string }
    | { kind: "EXEC_TEXT_ERROR_CWD_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; command: string; cwd: string }
    | { kind: "EXEC_TEXT_ERROR_CWD_NOT_A_FOLDER"; code: "ENOTDIR"; message: string; syscall: string; command: string; cwd: string }
    | { kind: "EXEC_TEXT_ERROR_NO_PERMISSION"; code: "EACCES" | "EPERM"; message: string; syscall: string; command: string }
    | { kind: "EXEC_TEXT_ERROR_COMMAND_LINE_TOO_LONG"; code: "E2BIG"; message: string; syscall: string; command: string }
    | { kind: "EXEC_TEXT_ERROR_TOO_MANY_FILES"; code: "EMFILE"; message: string; syscall: string; command: string }
    | { kind: "EXEC_TEXT_ERROR_TOO_MANY_PROCESSES"; code: "EAGAIN"; message: string; syscall: string; command: string }
    | { kind: "EXEC_TEXT_ERROR_INPUT_CLOSED"; code: "EPIPE" | "EOF"; message: string; syscall: string; command: string; stdout?: string; stderr?: string }
    | { kind: "EXEC_TEXT_ERROR_TIMEOUT"; code: "ETIMEDOUT"; message: string; command: string; timeout: int; signal: string; stdout?: string; stderr?: string }
    | { kind: "EXEC_TEXT_ERROR_OUTPUT_TOO_LARGE"; code: "ENOBUFS"; message: string; command: string; stream: "stdout" | "stderr"; maxBuffer: int; signal: string; stdout?: string; stderr?: string }
    | { kind: "EXEC_TEXT_ERROR_EXIT"; code: "ERR_NONZERO_EXIT"; message: string; command: string; status: int; stdout?: string; stderr?: string }
    | { kind: "EXEC_TEXT_ERROR_SHELL_COMMAND_NOT_FOUND"; code: "ERR_COMMAND_NOT_FOUND"; message: string; command: string; stdout?: string; stderr?: string }
    | { kind: "EXEC_TEXT_ERROR_SHELL_COMMAND_NOT_RUNNABLE"; code: "ERR_COMMAND_NOT_RUNNABLE"; message: string; command: string; stdout?: string; stderr?: string }
    | { kind: "EXEC_TEXT_ERROR_CRASHED"; code: "ERR_CRASHED"; message: string; command: string; crash: | "SIGSEGV" | "SIGBUS" | "SIGILL" | "SIGFPE" | "SIGABRT" | "ACCESS_VIOLATION" | "STACK_OVERFLOW" | "STACK_BUFFER_OVERRUN" | "ILLEGAL_INSTRUCTION" | "INTEGER_DIVIDE_BY_ZERO" | "HEAP_CORRUPTION"; stdout?: string; stderr?: string }
    | { kind: "EXEC_TEXT_ERROR_STOPPED"; code: "ERR_STOPPED_BY_SIGNAL"; message: string; command: string; signal: "SIGTERM" | "SIGINT" | "SIGHUP" | "SIGQUIT"; stdout?: string; stderr?: string }
    | { kind: "EXEC_TEXT_ERROR_KILLED"; code: "ERR_KILLED_BY_SIGNAL"; message: string; command: string; signal: string; stdout?: string; stderr?: string }
    | { kind: "EXEC_TEXT_ERROR_ZERO_CHARACTER"; code: "ERR_INVALID_ARG_VALUE"; message: string; argument: string }
    | { kind: "EXEC_TEXT_ERROR_OUT_OF_RANGE"; code: "ERR_OUT_OF_RANGE"; message: string; argument: "timeout" | "maxBuffer"; value: double }
    | { kind: "EXEC_TEXT_ERROR_UNKNOWN_SIGNAL"; code: "ERR_UNKNOWN_SIGNAL"; message: string; signal: string }
    | { kind: "EXEC_TEXT_ERROR_OUT_OF_MEMORY"; code: "ENOMEM"; message: string; syscall: string; command: string }
    | { kind: "EXEC_TEXT_ERROR_SYSTEM"; code: "EIO" | "EFAULT" | "UNKNOWN"; message: string; syscall: string; command: string }
  export type ExecBytesError =
    | { kind: "EXEC_BYTES_ERROR_SHELL_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; command: string; shell: string }
    | { kind: "EXEC_BYTES_ERROR_CWD_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; command: string; cwd: string }
    | { kind: "EXEC_BYTES_ERROR_CWD_NOT_A_FOLDER"; code: "ENOTDIR"; message: string; syscall: string; command: string; cwd: string }
    | { kind: "EXEC_BYTES_ERROR_NO_PERMISSION"; code: "EACCES" | "EPERM"; message: string; syscall: string; command: string }
    | { kind: "EXEC_BYTES_ERROR_COMMAND_LINE_TOO_LONG"; code: "E2BIG"; message: string; syscall: string; command: string }
    | { kind: "EXEC_BYTES_ERROR_TOO_MANY_FILES"; code: "EMFILE"; message: string; syscall: string; command: string }
    | { kind: "EXEC_BYTES_ERROR_TOO_MANY_PROCESSES"; code: "EAGAIN"; message: string; syscall: string; command: string }
    | { kind: "EXEC_BYTES_ERROR_INPUT_CLOSED"; code: "EPIPE" | "EOF"; message: string; syscall: string; command: string; stdout?: Buffer; stderr?: Buffer }
    | { kind: "EXEC_BYTES_ERROR_TIMEOUT"; code: "ETIMEDOUT"; message: string; command: string; timeout: int; signal: string; stdout?: Buffer; stderr?: Buffer }
    | { kind: "EXEC_BYTES_ERROR_OUTPUT_TOO_LARGE"; code: "ENOBUFS"; message: string; command: string; stream: "stdout" | "stderr"; maxBuffer: int; signal: string; stdout?: Buffer; stderr?: Buffer }
    | { kind: "EXEC_BYTES_ERROR_EXIT"; code: "ERR_NONZERO_EXIT"; message: string; command: string; status: int; stdout?: Buffer; stderr?: Buffer }
    | { kind: "EXEC_BYTES_ERROR_SHELL_COMMAND_NOT_FOUND"; code: "ERR_COMMAND_NOT_FOUND"; message: string; command: string; stdout?: Buffer; stderr?: Buffer }
    | { kind: "EXEC_BYTES_ERROR_SHELL_COMMAND_NOT_RUNNABLE"; code: "ERR_COMMAND_NOT_RUNNABLE"; message: string; command: string; stdout?: Buffer; stderr?: Buffer }
    | { kind: "EXEC_BYTES_ERROR_CRASHED"; code: "ERR_CRASHED"; message: string; command: string; crash: | "SIGSEGV" | "SIGBUS" | "SIGILL" | "SIGFPE" | "SIGABRT" | "ACCESS_VIOLATION" | "STACK_OVERFLOW" | "STACK_BUFFER_OVERRUN" | "ILLEGAL_INSTRUCTION" | "INTEGER_DIVIDE_BY_ZERO" | "HEAP_CORRUPTION"; stdout?: Buffer; stderr?: Buffer }
    | { kind: "EXEC_BYTES_ERROR_STOPPED"; code: "ERR_STOPPED_BY_SIGNAL"; message: string; command: string; signal: "SIGTERM" | "SIGINT" | "SIGHUP" | "SIGQUIT"; stdout?: Buffer; stderr?: Buffer }
    | { kind: "EXEC_BYTES_ERROR_KILLED"; code: "ERR_KILLED_BY_SIGNAL"; message: string; command: string; signal: string; stdout?: Buffer; stderr?: Buffer }
    | { kind: "EXEC_BYTES_ERROR_ZERO_CHARACTER"; code: "ERR_INVALID_ARG_VALUE"; message: string; argument: string }
    | { kind: "EXEC_BYTES_ERROR_OUT_OF_RANGE"; code: "ERR_OUT_OF_RANGE"; message: string; argument: "timeout" | "maxBuffer"; value: double }
    | { kind: "EXEC_BYTES_ERROR_UNKNOWN_SIGNAL"; code: "ERR_UNKNOWN_SIGNAL"; message: string; signal: string }
    | { kind: "EXEC_BYTES_ERROR_OUT_OF_MEMORY"; code: "ENOMEM"; message: string; syscall: string; command: string }
    | { kind: "EXEC_BYTES_ERROR_SYSTEM"; code: "EIO" | "EFAULT" | "UNKNOWN"; message: string; syscall: string; command: string }
  export type ExecFileTextError =
    | { kind: "EXEC_FILE_TEXT_ERROR_PROGRAM_NOT_FOUND_IN_PATH"; code: "ENOENT"; message: string; syscall: string; file: string; args: string[] }
    | { kind: "EXEC_FILE_TEXT_ERROR_PROGRAM_FILE_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; file: string; args: string[] }
    | { kind: "EXEC_FILE_TEXT_ERROR_CWD_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; file: string; args: string[]; cwd: string }
    | { kind: "EXEC_FILE_TEXT_ERROR_CWD_NOT_A_FOLDER"; code: "ENOTDIR"; message: string; syscall: string; file: string; args: string[]; cwd: string }
    | { kind: "EXEC_FILE_TEXT_ERROR_NO_PERMISSION"; code: "EACCES" | "EPERM"; message: string; syscall: string; file: string; args: string[] }
    | { kind: "EXEC_FILE_TEXT_ERROR_NEEDS_ADMINISTRATOR"; code: "EACCES"; message: string; syscall: string; file: string; args: string[] }
    | { kind: "EXEC_FILE_TEXT_ERROR_BATCH_FILE_WITHOUT_SHELL"; code: "EINVAL"; message: string; syscall: string; file: string; args: string[] }
    | { kind: "EXEC_FILE_TEXT_ERROR_NOT_A_PROGRAM"; code: "EFTYPE" | "ENOEXEC"; message: string; syscall: string; file: string; args: string[] }
    | { kind: "EXEC_FILE_TEXT_ERROR_COMMAND_LINE_TOO_LONG"; code: "E2BIG"; message: string; syscall: string; file: string; args: string[] }
    | { kind: "EXEC_FILE_TEXT_ERROR_PATH_TOO_LONG"; code: "ENAMETOOLONG"; message: string; syscall: string; file: string; args: string[] }
    | { kind: "EXEC_FILE_TEXT_ERROR_TOO_MANY_FILES"; code: "EMFILE"; message: string; syscall: string; file: string; args: string[] }
    | { kind: "EXEC_FILE_TEXT_ERROR_TOO_MANY_PROCESSES"; code: "EAGAIN"; message: string; syscall: string; file: string; args: string[] }
    | { kind: "EXEC_FILE_TEXT_ERROR_INPUT_CLOSED"; code: "EPIPE" | "EOF"; message: string; syscall: string; file: string; args: string[]; stdout?: string; stderr?: string }
    | { kind: "EXEC_FILE_TEXT_ERROR_TIMEOUT"; code: "ETIMEDOUT"; message: string; file: string; args: string[]; timeout: int; signal: string; stdout?: string; stderr?: string }
    | { kind: "EXEC_FILE_TEXT_ERROR_OUTPUT_TOO_LARGE"; code: "ENOBUFS"; message: string; file: string; args: string[]; stream: "stdout" | "stderr"; maxBuffer: int; signal: string; stdout?: string; stderr?: string }
    | { kind: "EXEC_FILE_TEXT_ERROR_EXIT"; code: "ERR_NONZERO_EXIT"; message: string; file: string; args: string[]; status: int; stdout?: string; stderr?: string }
    | { kind: "EXEC_FILE_TEXT_ERROR_CRASHED"; code: "ERR_CRASHED"; message: string; file: string; args: string[]; crash: | "SIGSEGV" | "SIGBUS" | "SIGILL" | "SIGFPE" | "SIGABRT" | "ACCESS_VIOLATION" | "STACK_OVERFLOW" | "STACK_BUFFER_OVERRUN" | "ILLEGAL_INSTRUCTION" | "INTEGER_DIVIDE_BY_ZERO" | "HEAP_CORRUPTION"; stdout?: string; stderr?: string }
    | { kind: "EXEC_FILE_TEXT_ERROR_STOPPED"; code: "ERR_STOPPED_BY_SIGNAL"; message: string; file: string; args: string[]; signal: "SIGTERM" | "SIGINT" | "SIGHUP" | "SIGQUIT"; stdout?: string; stderr?: string }
    | { kind: "EXEC_FILE_TEXT_ERROR_KILLED"; code: "ERR_KILLED_BY_SIGNAL"; message: string; file: string; args: string[]; signal: string; stdout?: string; stderr?: string }
    | { kind: "EXEC_FILE_TEXT_ERROR_ZERO_CHARACTER"; code: "ERR_INVALID_ARG_VALUE"; message: string; argument: string }
    | { kind: "EXEC_FILE_TEXT_ERROR_EMPTY_NAME"; code: "ERR_INVALID_ARG_VALUE"; message: string }
    | { kind: "EXEC_FILE_TEXT_ERROR_OUT_OF_RANGE"; code: "ERR_OUT_OF_RANGE"; message: string; argument: "timeout" | "maxBuffer"; value: double }
    | { kind: "EXEC_FILE_TEXT_ERROR_UNKNOWN_SIGNAL"; code: "ERR_UNKNOWN_SIGNAL"; message: string; signal: string }
    | { kind: "EXEC_FILE_TEXT_ERROR_OUT_OF_MEMORY"; code: "ENOMEM"; message: string; syscall: string; file: string; args: string[] }
    | { kind: "EXEC_FILE_TEXT_ERROR_SYSTEM"; code: "EIO" | "EFAULT" | "UNKNOWN"; message: string; syscall: string; file: string; args: string[] }
  export type ExecFileBytesError =
    | { kind: "EXEC_FILE_BYTES_ERROR_PROGRAM_NOT_FOUND_IN_PATH"; code: "ENOENT"; message: string; syscall: string; file: string; args: string[] }
    | { kind: "EXEC_FILE_BYTES_ERROR_PROGRAM_FILE_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; file: string; args: string[] }
    | { kind: "EXEC_FILE_BYTES_ERROR_CWD_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; file: string; args: string[]; cwd: string }
    | { kind: "EXEC_FILE_BYTES_ERROR_CWD_NOT_A_FOLDER"; code: "ENOTDIR"; message: string; syscall: string; file: string; args: string[]; cwd: string }
    | { kind: "EXEC_FILE_BYTES_ERROR_NO_PERMISSION"; code: "EACCES" | "EPERM"; message: string; syscall: string; file: string; args: string[] }
    | { kind: "EXEC_FILE_BYTES_ERROR_NEEDS_ADMINISTRATOR"; code: "EACCES"; message: string; syscall: string; file: string; args: string[] }
    | { kind: "EXEC_FILE_BYTES_ERROR_BATCH_FILE_WITHOUT_SHELL"; code: "EINVAL"; message: string; syscall: string; file: string; args: string[] }
    | { kind: "EXEC_FILE_BYTES_ERROR_NOT_A_PROGRAM"; code: "EFTYPE" | "ENOEXEC"; message: string; syscall: string; file: string; args: string[] }
    | { kind: "EXEC_FILE_BYTES_ERROR_COMMAND_LINE_TOO_LONG"; code: "E2BIG"; message: string; syscall: string; file: string; args: string[] }
    | { kind: "EXEC_FILE_BYTES_ERROR_PATH_TOO_LONG"; code: "ENAMETOOLONG"; message: string; syscall: string; file: string; args: string[] }
    | { kind: "EXEC_FILE_BYTES_ERROR_TOO_MANY_FILES"; code: "EMFILE"; message: string; syscall: string; file: string; args: string[] }
    | { kind: "EXEC_FILE_BYTES_ERROR_TOO_MANY_PROCESSES"; code: "EAGAIN"; message: string; syscall: string; file: string; args: string[] }
    | { kind: "EXEC_FILE_BYTES_ERROR_INPUT_CLOSED"; code: "EPIPE" | "EOF"; message: string; syscall: string; file: string; args: string[]; stdout?: Buffer; stderr?: Buffer }
    | { kind: "EXEC_FILE_BYTES_ERROR_TIMEOUT"; code: "ETIMEDOUT"; message: string; file: string; args: string[]; timeout: int; signal: string; stdout?: Buffer; stderr?: Buffer }
    | { kind: "EXEC_FILE_BYTES_ERROR_OUTPUT_TOO_LARGE"; code: "ENOBUFS"; message: string; file: string; args: string[]; stream: "stdout" | "stderr"; maxBuffer: int; signal: string; stdout?: Buffer; stderr?: Buffer }
    | { kind: "EXEC_FILE_BYTES_ERROR_EXIT"; code: "ERR_NONZERO_EXIT"; message: string; file: string; args: string[]; status: int; stdout?: Buffer; stderr?: Buffer }
    | { kind: "EXEC_FILE_BYTES_ERROR_CRASHED"; code: "ERR_CRASHED"; message: string; file: string; args: string[]; crash: | "SIGSEGV" | "SIGBUS" | "SIGILL" | "SIGFPE" | "SIGABRT" | "ACCESS_VIOLATION" | "STACK_OVERFLOW" | "STACK_BUFFER_OVERRUN" | "ILLEGAL_INSTRUCTION" | "INTEGER_DIVIDE_BY_ZERO" | "HEAP_CORRUPTION"; stdout?: Buffer; stderr?: Buffer }
    | { kind: "EXEC_FILE_BYTES_ERROR_STOPPED"; code: "ERR_STOPPED_BY_SIGNAL"; message: string; file: string; args: string[]; signal: "SIGTERM" | "SIGINT" | "SIGHUP" | "SIGQUIT"; stdout?: Buffer; stderr?: Buffer }
    | { kind: "EXEC_FILE_BYTES_ERROR_KILLED"; code: "ERR_KILLED_BY_SIGNAL"; message: string; file: string; args: string[]; signal: string; stdout?: Buffer; stderr?: Buffer }
    | { kind: "EXEC_FILE_BYTES_ERROR_ZERO_CHARACTER"; code: "ERR_INVALID_ARG_VALUE"; message: string; argument: string }
    | { kind: "EXEC_FILE_BYTES_ERROR_EMPTY_NAME"; code: "ERR_INVALID_ARG_VALUE"; message: string }
    | { kind: "EXEC_FILE_BYTES_ERROR_OUT_OF_RANGE"; code: "ERR_OUT_OF_RANGE"; message: string; argument: "timeout" | "maxBuffer"; value: double }
    | { kind: "EXEC_FILE_BYTES_ERROR_UNKNOWN_SIGNAL"; code: "ERR_UNKNOWN_SIGNAL"; message: string; signal: string }
    | { kind: "EXEC_FILE_BYTES_ERROR_OUT_OF_MEMORY"; code: "ENOMEM"; message: string; syscall: string; file: string; args: string[] }
    | { kind: "EXEC_FILE_BYTES_ERROR_SYSTEM"; code: "EIO" | "EFAULT" | "UNKNOWN"; message: string; syscall: string; file: string; args: string[] }
  export type SpawnTextError =
    | { kind: "SPAWN_TEXT_ERROR_PROGRAM_NOT_FOUND_IN_PATH"; code: "ENOENT"; message: string; syscall: string; command: string; args: string[] }
    | { kind: "SPAWN_TEXT_ERROR_PROGRAM_FILE_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; command: string; args: string[] }
    | { kind: "SPAWN_TEXT_ERROR_CWD_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; command: string; args: string[]; cwd: string }
    | { kind: "SPAWN_TEXT_ERROR_CWD_NOT_A_FOLDER"; code: "ENOTDIR"; message: string; syscall: string; command: string; args: string[]; cwd: string }
    | { kind: "SPAWN_TEXT_ERROR_NO_PERMISSION"; code: "EACCES" | "EPERM"; message: string; syscall: string; command: string; args: string[] }
    | { kind: "SPAWN_TEXT_ERROR_NEEDS_ADMINISTRATOR"; code: "EACCES"; message: string; syscall: string; command: string; args: string[] }
    | { kind: "SPAWN_TEXT_ERROR_BATCH_FILE_WITHOUT_SHELL"; code: "EINVAL"; message: string; syscall: string; command: string; args: string[] }
    | { kind: "SPAWN_TEXT_ERROR_NOT_A_PROGRAM"; code: "EFTYPE" | "ENOEXEC"; message: string; syscall: string; command: string; args: string[] }
    | { kind: "SPAWN_TEXT_ERROR_COMMAND_LINE_TOO_LONG"; code: "E2BIG"; message: string; syscall: string; command: string; args: string[] }
    | { kind: "SPAWN_TEXT_ERROR_PATH_TOO_LONG"; code: "ENAMETOOLONG"; message: string; syscall: string; command: string; args: string[] }
    | { kind: "SPAWN_TEXT_ERROR_TOO_MANY_FILES"; code: "EMFILE"; message: string; syscall: string; command: string; args: string[] }
    | { kind: "SPAWN_TEXT_ERROR_TOO_MANY_PROCESSES"; code: "EAGAIN"; message: string; syscall: string; command: string; args: string[] }
    | { kind: "SPAWN_TEXT_ERROR_INPUT_CLOSED"; code: "EPIPE" | "EOF"; message: string; syscall: string; command: string; args: string[]; stdout?: string; stderr?: string }
    | { kind: "SPAWN_TEXT_ERROR_TIMEOUT"; code: "ETIMEDOUT"; message: string; command: string; args: string[]; timeout: int; signal: string; stdout?: string; stderr?: string }
    | { kind: "SPAWN_TEXT_ERROR_OUTPUT_TOO_LARGE"; code: "ENOBUFS"; message: string; command: string; args: string[]; stream: "stdout" | "stderr"; maxBuffer: int; signal: string; stdout?: string; stderr?: string }
    | { kind: "SPAWN_TEXT_ERROR_ZERO_CHARACTER"; code: "ERR_INVALID_ARG_VALUE"; message: string; argument: string }
    | { kind: "SPAWN_TEXT_ERROR_EMPTY_NAME"; code: "ERR_INVALID_ARG_VALUE"; message: string }
    | { kind: "SPAWN_TEXT_ERROR_OUT_OF_RANGE"; code: "ERR_OUT_OF_RANGE"; message: string; argument: "timeout" | "maxBuffer"; value: double }
    | { kind: "SPAWN_TEXT_ERROR_UNKNOWN_SIGNAL"; code: "ERR_UNKNOWN_SIGNAL"; message: string; signal: string }
    | { kind: "SPAWN_TEXT_ERROR_OUT_OF_MEMORY"; code: "ENOMEM"; message: string; syscall: string; command: string; args: string[] }
    | { kind: "SPAWN_TEXT_ERROR_SYSTEM"; code: "EIO" | "EFAULT" | "UNKNOWN"; message: string; syscall: string; command: string; args: string[] }
  export type SpawnBytesError =
    | { kind: "SPAWN_BYTES_ERROR_PROGRAM_NOT_FOUND_IN_PATH"; code: "ENOENT"; message: string; syscall: string; command: string; args: string[] }
    | { kind: "SPAWN_BYTES_ERROR_PROGRAM_FILE_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; command: string; args: string[] }
    | { kind: "SPAWN_BYTES_ERROR_CWD_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; command: string; args: string[]; cwd: string }
    | { kind: "SPAWN_BYTES_ERROR_CWD_NOT_A_FOLDER"; code: "ENOTDIR"; message: string; syscall: string; command: string; args: string[]; cwd: string }
    | { kind: "SPAWN_BYTES_ERROR_NO_PERMISSION"; code: "EACCES" | "EPERM"; message: string; syscall: string; command: string; args: string[] }
    | { kind: "SPAWN_BYTES_ERROR_NEEDS_ADMINISTRATOR"; code: "EACCES"; message: string; syscall: string; command: string; args: string[] }
    | { kind: "SPAWN_BYTES_ERROR_BATCH_FILE_WITHOUT_SHELL"; code: "EINVAL"; message: string; syscall: string; command: string; args: string[] }
    | { kind: "SPAWN_BYTES_ERROR_NOT_A_PROGRAM"; code: "EFTYPE" | "ENOEXEC"; message: string; syscall: string; command: string; args: string[] }
    | { kind: "SPAWN_BYTES_ERROR_COMMAND_LINE_TOO_LONG"; code: "E2BIG"; message: string; syscall: string; command: string; args: string[] }
    | { kind: "SPAWN_BYTES_ERROR_PATH_TOO_LONG"; code: "ENAMETOOLONG"; message: string; syscall: string; command: string; args: string[] }
    | { kind: "SPAWN_BYTES_ERROR_TOO_MANY_FILES"; code: "EMFILE"; message: string; syscall: string; command: string; args: string[] }
    | { kind: "SPAWN_BYTES_ERROR_TOO_MANY_PROCESSES"; code: "EAGAIN"; message: string; syscall: string; command: string; args: string[] }
    | { kind: "SPAWN_BYTES_ERROR_INPUT_CLOSED"; code: "EPIPE" | "EOF"; message: string; syscall: string; command: string; args: string[]; stdout?: Buffer; stderr?: Buffer }
    | { kind: "SPAWN_BYTES_ERROR_TIMEOUT"; code: "ETIMEDOUT"; message: string; command: string; args: string[]; timeout: int; signal: string; stdout?: Buffer; stderr?: Buffer }
    | { kind: "SPAWN_BYTES_ERROR_OUTPUT_TOO_LARGE"; code: "ENOBUFS"; message: string; command: string; args: string[]; stream: "stdout" | "stderr"; maxBuffer: int; signal: string; stdout?: Buffer; stderr?: Buffer }
    | { kind: "SPAWN_BYTES_ERROR_ZERO_CHARACTER"; code: "ERR_INVALID_ARG_VALUE"; message: string; argument: string }
    | { kind: "SPAWN_BYTES_ERROR_EMPTY_NAME"; code: "ERR_INVALID_ARG_VALUE"; message: string }
    | { kind: "SPAWN_BYTES_ERROR_OUT_OF_RANGE"; code: "ERR_OUT_OF_RANGE"; message: string; argument: "timeout" | "maxBuffer"; value: double }
    | { kind: "SPAWN_BYTES_ERROR_UNKNOWN_SIGNAL"; code: "ERR_UNKNOWN_SIGNAL"; message: string; signal: string }
    | { kind: "SPAWN_BYTES_ERROR_OUT_OF_MEMORY"; code: "ENOMEM"; message: string; syscall: string; command: string; args: string[] }
    | { kind: "SPAWN_BYTES_ERROR_SYSTEM"; code: "EIO" | "EFAULT" | "UNKNOWN"; message: string; syscall: string; command: string; args: string[] }
  export type ExecFileShellTextError =
    | { kind: "EXEC_FILE_SHELL_TEXT_ERROR_SHELL_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; file: string; args: string[]; shell: string }
    | { kind: "EXEC_FILE_SHELL_TEXT_ERROR_CWD_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; file: string; args: string[]; cwd: string }
    | { kind: "EXEC_FILE_SHELL_TEXT_ERROR_CWD_NOT_A_FOLDER"; code: "ENOTDIR"; message: string; syscall: string; file: string; args: string[]; cwd: string }
    | { kind: "EXEC_FILE_SHELL_TEXT_ERROR_NO_PERMISSION"; code: "EACCES" | "EPERM"; message: string; syscall: string; file: string; args: string[] }
    | { kind: "EXEC_FILE_SHELL_TEXT_ERROR_COMMAND_LINE_TOO_LONG"; code: "E2BIG"; message: string; syscall: string; file: string; args: string[] }
    | { kind: "EXEC_FILE_SHELL_TEXT_ERROR_TOO_MANY_FILES"; code: "EMFILE"; message: string; syscall: string; file: string; args: string[] }
    | { kind: "EXEC_FILE_SHELL_TEXT_ERROR_TOO_MANY_PROCESSES"; code: "EAGAIN"; message: string; syscall: string; file: string; args: string[] }
    | { kind: "EXEC_FILE_SHELL_TEXT_ERROR_INPUT_CLOSED"; code: "EPIPE" | "EOF"; message: string; syscall: string; file: string; args: string[]; stdout?: string; stderr?: string }
    | { kind: "EXEC_FILE_SHELL_TEXT_ERROR_TIMEOUT"; code: "ETIMEDOUT"; message: string; file: string; args: string[]; timeout: int; signal: string; stdout?: string; stderr?: string }
    | { kind: "EXEC_FILE_SHELL_TEXT_ERROR_OUTPUT_TOO_LARGE"; code: "ENOBUFS"; message: string; file: string; args: string[]; stream: "stdout" | "stderr"; maxBuffer: int; signal: string; stdout?: string; stderr?: string }
    | { kind: "EXEC_FILE_SHELL_TEXT_ERROR_EXIT"; code: "ERR_NONZERO_EXIT"; message: string; file: string; args: string[]; status: int; stdout?: string; stderr?: string }
    | { kind: "EXEC_FILE_SHELL_TEXT_ERROR_SHELL_COMMAND_NOT_FOUND"; code: "ERR_COMMAND_NOT_FOUND"; message: string; file: string; args: string[]; stdout?: string; stderr?: string }
    | { kind: "EXEC_FILE_SHELL_TEXT_ERROR_SHELL_COMMAND_NOT_RUNNABLE"; code: "ERR_COMMAND_NOT_RUNNABLE"; message: string; file: string; args: string[]; stdout?: string; stderr?: string }
    | { kind: "EXEC_FILE_SHELL_TEXT_ERROR_CRASHED"; code: "ERR_CRASHED"; message: string; file: string; args: string[]; crash: | "SIGSEGV" | "SIGBUS" | "SIGILL" | "SIGFPE" | "SIGABRT" | "ACCESS_VIOLATION" | "STACK_OVERFLOW" | "STACK_BUFFER_OVERRUN" | "ILLEGAL_INSTRUCTION" | "INTEGER_DIVIDE_BY_ZERO" | "HEAP_CORRUPTION"; stdout?: string; stderr?: string }
    | { kind: "EXEC_FILE_SHELL_TEXT_ERROR_STOPPED"; code: "ERR_STOPPED_BY_SIGNAL"; message: string; file: string; args: string[]; signal: "SIGTERM" | "SIGINT" | "SIGHUP" | "SIGQUIT"; stdout?: string; stderr?: string }
    | { kind: "EXEC_FILE_SHELL_TEXT_ERROR_KILLED"; code: "ERR_KILLED_BY_SIGNAL"; message: string; file: string; args: string[]; signal: string; stdout?: string; stderr?: string }
    | { kind: "EXEC_FILE_SHELL_TEXT_ERROR_ZERO_CHARACTER"; code: "ERR_INVALID_ARG_VALUE"; message: string; argument: string }
    | { kind: "EXEC_FILE_SHELL_TEXT_ERROR_EMPTY_NAME"; code: "ERR_INVALID_ARG_VALUE"; message: string }
    | { kind: "EXEC_FILE_SHELL_TEXT_ERROR_OUT_OF_RANGE"; code: "ERR_OUT_OF_RANGE"; message: string; argument: "timeout" | "maxBuffer"; value: double }
    | { kind: "EXEC_FILE_SHELL_TEXT_ERROR_UNKNOWN_SIGNAL"; code: "ERR_UNKNOWN_SIGNAL"; message: string; signal: string }
    | { kind: "EXEC_FILE_SHELL_TEXT_ERROR_OUT_OF_MEMORY"; code: "ENOMEM"; message: string; syscall: string; file: string; args: string[] }
    | { kind: "EXEC_FILE_SHELL_TEXT_ERROR_SYSTEM"; code: "EIO" | "EFAULT" | "UNKNOWN"; message: string; syscall: string; file: string; args: string[] }
  export type ExecFileShellBytesError =
    | { kind: "EXEC_FILE_SHELL_BYTES_ERROR_SHELL_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; file: string; args: string[]; shell: string }
    | { kind: "EXEC_FILE_SHELL_BYTES_ERROR_CWD_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; file: string; args: string[]; cwd: string }
    | { kind: "EXEC_FILE_SHELL_BYTES_ERROR_CWD_NOT_A_FOLDER"; code: "ENOTDIR"; message: string; syscall: string; file: string; args: string[]; cwd: string }
    | { kind: "EXEC_FILE_SHELL_BYTES_ERROR_NO_PERMISSION"; code: "EACCES" | "EPERM"; message: string; syscall: string; file: string; args: string[] }
    | { kind: "EXEC_FILE_SHELL_BYTES_ERROR_COMMAND_LINE_TOO_LONG"; code: "E2BIG"; message: string; syscall: string; file: string; args: string[] }
    | { kind: "EXEC_FILE_SHELL_BYTES_ERROR_TOO_MANY_FILES"; code: "EMFILE"; message: string; syscall: string; file: string; args: string[] }
    | { kind: "EXEC_FILE_SHELL_BYTES_ERROR_TOO_MANY_PROCESSES"; code: "EAGAIN"; message: string; syscall: string; file: string; args: string[] }
    | { kind: "EXEC_FILE_SHELL_BYTES_ERROR_INPUT_CLOSED"; code: "EPIPE" | "EOF"; message: string; syscall: string; file: string; args: string[]; stdout?: Buffer; stderr?: Buffer }
    | { kind: "EXEC_FILE_SHELL_BYTES_ERROR_TIMEOUT"; code: "ETIMEDOUT"; message: string; file: string; args: string[]; timeout: int; signal: string; stdout?: Buffer; stderr?: Buffer }
    | { kind: "EXEC_FILE_SHELL_BYTES_ERROR_OUTPUT_TOO_LARGE"; code: "ENOBUFS"; message: string; file: string; args: string[]; stream: "stdout" | "stderr"; maxBuffer: int; signal: string; stdout?: Buffer; stderr?: Buffer }
    | { kind: "EXEC_FILE_SHELL_BYTES_ERROR_EXIT"; code: "ERR_NONZERO_EXIT"; message: string; file: string; args: string[]; status: int; stdout?: Buffer; stderr?: Buffer }
    | { kind: "EXEC_FILE_SHELL_BYTES_ERROR_SHELL_COMMAND_NOT_FOUND"; code: "ERR_COMMAND_NOT_FOUND"; message: string; file: string; args: string[]; stdout?: Buffer; stderr?: Buffer }
    | { kind: "EXEC_FILE_SHELL_BYTES_ERROR_SHELL_COMMAND_NOT_RUNNABLE"; code: "ERR_COMMAND_NOT_RUNNABLE"; message: string; file: string; args: string[]; stdout?: Buffer; stderr?: Buffer }
    | { kind: "EXEC_FILE_SHELL_BYTES_ERROR_CRASHED"; code: "ERR_CRASHED"; message: string; file: string; args: string[]; crash: | "SIGSEGV" | "SIGBUS" | "SIGILL" | "SIGFPE" | "SIGABRT" | "ACCESS_VIOLATION" | "STACK_OVERFLOW" | "STACK_BUFFER_OVERRUN" | "ILLEGAL_INSTRUCTION" | "INTEGER_DIVIDE_BY_ZERO" | "HEAP_CORRUPTION"; stdout?: Buffer; stderr?: Buffer }
    | { kind: "EXEC_FILE_SHELL_BYTES_ERROR_STOPPED"; code: "ERR_STOPPED_BY_SIGNAL"; message: string; file: string; args: string[]; signal: "SIGTERM" | "SIGINT" | "SIGHUP" | "SIGQUIT"; stdout?: Buffer; stderr?: Buffer }
    | { kind: "EXEC_FILE_SHELL_BYTES_ERROR_KILLED"; code: "ERR_KILLED_BY_SIGNAL"; message: string; file: string; args: string[]; signal: string; stdout?: Buffer; stderr?: Buffer }
    | { kind: "EXEC_FILE_SHELL_BYTES_ERROR_ZERO_CHARACTER"; code: "ERR_INVALID_ARG_VALUE"; message: string; argument: string }
    | { kind: "EXEC_FILE_SHELL_BYTES_ERROR_EMPTY_NAME"; code: "ERR_INVALID_ARG_VALUE"; message: string }
    | { kind: "EXEC_FILE_SHELL_BYTES_ERROR_OUT_OF_RANGE"; code: "ERR_OUT_OF_RANGE"; message: string; argument: "timeout" | "maxBuffer"; value: double }
    | { kind: "EXEC_FILE_SHELL_BYTES_ERROR_UNKNOWN_SIGNAL"; code: "ERR_UNKNOWN_SIGNAL"; message: string; signal: string }
    | { kind: "EXEC_FILE_SHELL_BYTES_ERROR_OUT_OF_MEMORY"; code: "ENOMEM"; message: string; syscall: string; file: string; args: string[] }
    | { kind: "EXEC_FILE_SHELL_BYTES_ERROR_SYSTEM"; code: "EIO" | "EFAULT" | "UNKNOWN"; message: string; syscall: string; file: string; args: string[] }
  export type SpawnShellTextError =
    | { kind: "SPAWN_SHELL_TEXT_ERROR_SHELL_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; command: string; args: string[]; shell: string }
    | { kind: "SPAWN_SHELL_TEXT_ERROR_CWD_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; command: string; args: string[]; cwd: string }
    | { kind: "SPAWN_SHELL_TEXT_ERROR_CWD_NOT_A_FOLDER"; code: "ENOTDIR"; message: string; syscall: string; command: string; args: string[]; cwd: string }
    | { kind: "SPAWN_SHELL_TEXT_ERROR_NO_PERMISSION"; code: "EACCES" | "EPERM"; message: string; syscall: string; command: string; args: string[] }
    | { kind: "SPAWN_SHELL_TEXT_ERROR_COMMAND_LINE_TOO_LONG"; code: "E2BIG"; message: string; syscall: string; command: string; args: string[] }
    | { kind: "SPAWN_SHELL_TEXT_ERROR_TOO_MANY_FILES"; code: "EMFILE"; message: string; syscall: string; command: string; args: string[] }
    | { kind: "SPAWN_SHELL_TEXT_ERROR_TOO_MANY_PROCESSES"; code: "EAGAIN"; message: string; syscall: string; command: string; args: string[] }
    | { kind: "SPAWN_SHELL_TEXT_ERROR_INPUT_CLOSED"; code: "EPIPE" | "EOF"; message: string; syscall: string; command: string; args: string[]; stdout?: string; stderr?: string }
    | { kind: "SPAWN_SHELL_TEXT_ERROR_TIMEOUT"; code: "ETIMEDOUT"; message: string; command: string; args: string[]; timeout: int; signal: string; stdout?: string; stderr?: string }
    | { kind: "SPAWN_SHELL_TEXT_ERROR_OUTPUT_TOO_LARGE"; code: "ENOBUFS"; message: string; command: string; args: string[]; stream: "stdout" | "stderr"; maxBuffer: int; signal: string; stdout?: string; stderr?: string }
    | { kind: "SPAWN_SHELL_TEXT_ERROR_ZERO_CHARACTER"; code: "ERR_INVALID_ARG_VALUE"; message: string; argument: string }
    | { kind: "SPAWN_SHELL_TEXT_ERROR_EMPTY_NAME"; code: "ERR_INVALID_ARG_VALUE"; message: string }
    | { kind: "SPAWN_SHELL_TEXT_ERROR_OUT_OF_RANGE"; code: "ERR_OUT_OF_RANGE"; message: string; argument: "timeout" | "maxBuffer"; value: double }
    | { kind: "SPAWN_SHELL_TEXT_ERROR_UNKNOWN_SIGNAL"; code: "ERR_UNKNOWN_SIGNAL"; message: string; signal: string }
    | { kind: "SPAWN_SHELL_TEXT_ERROR_OUT_OF_MEMORY"; code: "ENOMEM"; message: string; syscall: string; command: string; args: string[] }
    | { kind: "SPAWN_SHELL_TEXT_ERROR_SYSTEM"; code: "EIO" | "EFAULT" | "UNKNOWN"; message: string; syscall: string; command: string; args: string[] }
  export type SpawnShellBytesError =
    | { kind: "SPAWN_SHELL_BYTES_ERROR_SHELL_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; command: string; args: string[]; shell: string }
    | { kind: "SPAWN_SHELL_BYTES_ERROR_CWD_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; command: string; args: string[]; cwd: string }
    | { kind: "SPAWN_SHELL_BYTES_ERROR_CWD_NOT_A_FOLDER"; code: "ENOTDIR"; message: string; syscall: string; command: string; args: string[]; cwd: string }
    | { kind: "SPAWN_SHELL_BYTES_ERROR_NO_PERMISSION"; code: "EACCES" | "EPERM"; message: string; syscall: string; command: string; args: string[] }
    | { kind: "SPAWN_SHELL_BYTES_ERROR_COMMAND_LINE_TOO_LONG"; code: "E2BIG"; message: string; syscall: string; command: string; args: string[] }
    | { kind: "SPAWN_SHELL_BYTES_ERROR_TOO_MANY_FILES"; code: "EMFILE"; message: string; syscall: string; command: string; args: string[] }
    | { kind: "SPAWN_SHELL_BYTES_ERROR_TOO_MANY_PROCESSES"; code: "EAGAIN"; message: string; syscall: string; command: string; args: string[] }
    | { kind: "SPAWN_SHELL_BYTES_ERROR_INPUT_CLOSED"; code: "EPIPE" | "EOF"; message: string; syscall: string; command: string; args: string[]; stdout?: Buffer; stderr?: Buffer }
    | { kind: "SPAWN_SHELL_BYTES_ERROR_TIMEOUT"; code: "ETIMEDOUT"; message: string; command: string; args: string[]; timeout: int; signal: string; stdout?: Buffer; stderr?: Buffer }
    | { kind: "SPAWN_SHELL_BYTES_ERROR_OUTPUT_TOO_LARGE"; code: "ENOBUFS"; message: string; command: string; args: string[]; stream: "stdout" | "stderr"; maxBuffer: int; signal: string; stdout?: Buffer; stderr?: Buffer }
    | { kind: "SPAWN_SHELL_BYTES_ERROR_ZERO_CHARACTER"; code: "ERR_INVALID_ARG_VALUE"; message: string; argument: string }
    | { kind: "SPAWN_SHELL_BYTES_ERROR_EMPTY_NAME"; code: "ERR_INVALID_ARG_VALUE"; message: string }
    | { kind: "SPAWN_SHELL_BYTES_ERROR_OUT_OF_RANGE"; code: "ERR_OUT_OF_RANGE"; message: string; argument: "timeout" | "maxBuffer"; value: double }
    | { kind: "SPAWN_SHELL_BYTES_ERROR_UNKNOWN_SIGNAL"; code: "ERR_UNKNOWN_SIGNAL"; message: string; signal: string }
    | { kind: "SPAWN_SHELL_BYTES_ERROR_OUT_OF_MEMORY"; code: "ENOMEM"; message: string; syscall: string; command: string; args: string[] }
    | { kind: "SPAWN_SHELL_BYTES_ERROR_SYSTEM"; code: "EIO" | "EFAULT" | "UNKNOWN"; message: string; syscall: string; command: string; args: string[] }
  export type SpawnTextResult = { pid: int; status?: int; signal?: string; output: (string | undefined)[]; stdout?: string; stderr?: string }
  export type SpawnBytesResult = { pid: int; status?: int; signal?: string; output: (Buffer | undefined)[]; stdout?: Buffer; stderr?: Buffer }
  export function execSync(command: string, options: ExecSyncOptions & Text): Result<string, ExecTextError>
  export function execSync(command: string, options?: ExecSyncOptions & Bytes): Result<Buffer, ExecBytesError>
  export function execFileSync(file: string, args: readonly string[], options: ExecFileSyncOptions & Text & Shell): Result<string, ExecFileShellTextError>
  export function execFileSync(file: string, args: readonly string[], options: ExecFileSyncOptions & Bytes & Shell): Result<Buffer, ExecFileShellBytesError>
  export function execFileSync(file: string, options: ExecFileSyncOptions & Text): Result<string, ExecFileTextError>
  export function execFileSync(file: string, args: readonly string[], options: ExecFileSyncOptions & Text): Result<string, ExecFileTextError>
  export function execFileSync(file: string, options?: ExecFileSyncOptions & Bytes): Result<Buffer, ExecFileBytesError>
  export function execFileSync(file: string, args?: readonly string[], options?: ExecFileSyncOptions & Bytes): Result<Buffer, ExecFileBytesError>
  export function spawnSync(command: string, args: readonly string[], options: SpawnSyncOptions & Text & Shell): Result<SpawnTextResult, SpawnShellTextError>
  export function spawnSync(command: string, args: readonly string[], options: SpawnSyncOptions & Bytes & Shell): Result<SpawnBytesResult, SpawnShellBytesError>
  export function spawnSync(command: string, options: SpawnSyncOptions & Text): Result<SpawnTextResult, SpawnTextError>
  export function spawnSync(command: string, args: readonly string[], options: SpawnSyncOptions & Text): Result<SpawnTextResult, SpawnTextError>
  export function spawnSync(command: string, options?: SpawnSyncOptions & Bytes): Result<SpawnBytesResult, SpawnBytesError>
  export function spawnSync(command: string, args?: readonly string[], options?: SpawnSyncOptions & Bytes): Result<SpawnBytesResult, SpawnBytesError>
}

// node:crypto gives a Result where node throws (docs/crypto.md, "Ошибки"): a type of error
// for each function, kind = the type and the reason.
declare module "node:crypto" {
  type KeyReadCode =
    | "ERR_OSSL_UNSUPPORTED" | "ERR_OSSL_PEM_BAD_BASE64_DECODE" | "ERR_OSSL_PEM_BAD_DECRYPT"
    | "ERR_OSSL_PEM_BAD_END_LINE" | "ERR_OSSL_PEM_BAD_IV_CHARS" | "ERR_OSSL_PEM_BAD_MAGIC_NUMBER"
    | "ERR_OSSL_PEM_BAD_PASSWORD_READ" | "ERR_OSSL_PEM_BAD_VERSION_NUMBER"
    | "ERR_OSSL_PEM_BIO_WRITE_FAILURE" | "ERR_OSSL_PEM_CIPHER_IS_NULL"
    | "ERR_OSSL_PEM_ERROR_CONVERTING_PRIVATE_KEY" | "ERR_OSSL_PEM_EXPECTING_DSS_KEY_BLOB"
    | "ERR_OSSL_PEM_EXPECTING_PRIVATE_KEY_BLOB" | "ERR_OSSL_PEM_EXPECTING_PUBLIC_KEY_BLOB"
    | "ERR_OSSL_PEM_EXPECTING_RSA_KEY_BLOB" | "ERR_OSSL_PEM_HEADER_TOO_LONG"
    | "ERR_OSSL_PEM_INCONSISTENT_HEADER" | "ERR_OSSL_PEM_KEYBLOB_HEADER_PARSE_ERROR"
    | "ERR_OSSL_PEM_KEYBLOB_TOO_SHORT" | "ERR_OSSL_PEM_MISSING_DEK_IV" | "ERR_OSSL_PEM_NOT_DEK_INFO"
    | "ERR_OSSL_PEM_NOT_ENCRYPTED" | "ERR_OSSL_PEM_NOT_PROC_TYPE" | "ERR_OSSL_PEM_NO_START_LINE"
    | "ERR_OSSL_PEM_PROBLEMS_GETTING_PASSWORD" | "ERR_OSSL_PEM_PVK_DATA_TOO_SHORT"
    | "ERR_OSSL_PEM_PVK_TOO_SHORT" | "ERR_OSSL_PEM_READ_KEY" | "ERR_OSSL_PEM_SHORT_HEADER"
    | "ERR_OSSL_PEM_UNEXPECTED_DEK_IV" | "ERR_OSSL_PEM_UNSUPPORTED_CIPHER"
    | "ERR_OSSL_PEM_UNSUPPORTED_ENCRYPTION" | "ERR_OSSL_PEM_UNSUPPORTED_KEY_COMPONENTS"
    | "ERR_OSSL_PEM_UNSUPPORTED_PUBLIC_KEY_TYPE" | "ERR_OSSL_PEM_UNSUPPORTED_PVK_KEY_TYPE"
    | "ERR_OSSL_ASN1_ADDING_OBJECT" | "ERR_OSSL_ASN1_ASN1_PARSE_ERROR"
    | "ERR_OSSL_ASN1_ASN1_SIG_PARSE_ERROR" | "ERR_OSSL_ASN1_AUX_ERROR"
    | "ERR_OSSL_ASN1_BAD_OBJECT_HEADER" | "ERR_OSSL_ASN1_BAD_TEMPLATE"
    | "ERR_OSSL_ASN1_BMPSTRING_IS_WRONG_LENGTH" | "ERR_OSSL_ASN1_BN_LIB"
    | "ERR_OSSL_ASN1_BOOLEAN_IS_WRONG_LENGTH" | "ERR_OSSL_ASN1_BUFFER_TOO_SMALL"
    | "ERR_OSSL_ASN1_CIPHER_HAS_NO_OBJECT_IDENTIFIER" | "ERR_OSSL_ASN1_CONTEXT_NOT_INITIALISED"
    | "ERR_OSSL_ASN1_DATA_IS_WRONG" | "ERR_OSSL_ASN1_DECODE_ERROR" | "ERR_OSSL_ASN1_DEPTH_EXCEEDED"
    | "ERR_OSSL_ASN1_DIGEST_AND_KEY_TYPE_NOT_SUPPORTED" | "ERR_OSSL_ASN1_ENCODE_ERROR"
    | "ERR_OSSL_ASN1_ERROR_GETTING_TIME" | "ERR_OSSL_ASN1_ERROR_LOADING_SECTION"
    | "ERR_OSSL_ASN1_ERROR_SETTING_CIPHER_PARAMS" | "ERR_OSSL_ASN1_EXPECTING_AN_INTEGER"
    | "ERR_OSSL_ASN1_EXPECTING_AN_OBJECT" | "ERR_OSSL_ASN1_EXPLICIT_LENGTH_MISMATCH"
    | "ERR_OSSL_ASN1_EXPLICIT_TAG_NOT_CONSTRUCTED" | "ERR_OSSL_ASN1_FIELD_MISSING"
    | "ERR_OSSL_ASN1_FIRST_NUM_TOO_LARGE" | "ERR_OSSL_ASN1_GENERALIZEDTIME_IS_TOO_SHORT"
    | "ERR_OSSL_ASN1_HEADER_TOO_LONG" | "ERR_OSSL_ASN1_ILLEGAL_BITSTRING_FORMAT"
    | "ERR_OSSL_ASN1_ILLEGAL_BOOLEAN" | "ERR_OSSL_ASN1_ILLEGAL_CHARACTERS"
    | "ERR_OSSL_ASN1_ILLEGAL_FORMAT" | "ERR_OSSL_ASN1_ILLEGAL_HEX"
    | "ERR_OSSL_ASN1_ILLEGAL_IMPLICIT_TAG" | "ERR_OSSL_ASN1_ILLEGAL_INTEGER"
    | "ERR_OSSL_ASN1_ILLEGAL_NEGATIVE_VALUE" | "ERR_OSSL_ASN1_ILLEGAL_NESTED_TAGGING"
    | "ERR_OSSL_ASN1_ILLEGAL_NULL" | "ERR_OSSL_ASN1_ILLEGAL_NULL_VALUE"
    | "ERR_OSSL_ASN1_ILLEGAL_OBJECT" | "ERR_OSSL_ASN1_ILLEGAL_OPTIONAL_ANY"
    | "ERR_OSSL_ASN1_ILLEGAL_OPTIONS_ON_ITEM_TEMPLATE" | "ERR_OSSL_ASN1_ILLEGAL_PADDING"
    | "ERR_OSSL_ASN1_ILLEGAL_TAGGED_ANY" | "ERR_OSSL_ASN1_ILLEGAL_TIME_VALUE"
    | "ERR_OSSL_ASN1_ILLEGAL_ZERO_CONTENT" | "ERR_OSSL_ASN1_INTEGER_NOT_ASCII_FORMAT"
    | "ERR_OSSL_ASN1_INTEGER_TOO_LARGE_FOR_LONG" | "ERR_OSSL_ASN1_INVALID_BIT_STRING_BITS_LEFT"
    | "ERR_OSSL_ASN1_INVALID_BMPSTRING_LENGTH" | "ERR_OSSL_ASN1_INVALID_DIGIT"
    | "ERR_OSSL_ASN1_INVALID_MIME_TYPE" | "ERR_OSSL_ASN1_INVALID_MODIFIER"
    | "ERR_OSSL_ASN1_INVALID_NUMBER" | "ERR_OSSL_ASN1_INVALID_OBJECT_ENCODING"
    | "ERR_OSSL_ASN1_INVALID_SCRYPT_PARAMETERS" | "ERR_OSSL_ASN1_INVALID_SEPARATOR"
    | "ERR_OSSL_ASN1_INVALID_STRING_TABLE_VALUE" | "ERR_OSSL_ASN1_INVALID_UNIVERSALSTRING_LENGTH"
    | "ERR_OSSL_ASN1_INVALID_UTF8STRING" | "ERR_OSSL_ASN1_INVALID_VALUE"
    | "ERR_OSSL_ASN1_LENGTH_TOO_LONG" | "ERR_OSSL_ASN1_LIST_ERROR"
    | "ERR_OSSL_ASN1_MIME_NO_CONTENT_TYPE" | "ERR_OSSL_ASN1_MIME_PARSE_ERROR"
    | "ERR_OSSL_ASN1_MIME_SIG_PARSE_ERROR" | "ERR_OSSL_ASN1_MISSING_EOC"
    | "ERR_OSSL_ASN1_MISSING_SECOND_NUMBER" | "ERR_OSSL_ASN1_MISSING_VALUE"
    | "ERR_OSSL_ASN1_MSTRING_NOT_UNIVERSAL" | "ERR_OSSL_ASN1_MSTRING_WRONG_TAG"
    | "ERR_OSSL_ASN1_NESTED_ASN1_STRING" | "ERR_OSSL_ASN1_NESTED_TOO_DEEP"
    | "ERR_OSSL_ASN1_NON_HEX_CHARACTERS" | "ERR_OSSL_ASN1_NOT_ASCII_FORMAT"
    | "ERR_OSSL_ASN1_NOT_ENOUGH_DATA" | "ERR_OSSL_ASN1_NO_CONTENT_TYPE"
    | "ERR_OSSL_ASN1_NO_MATCHING_CHOICE_TYPE" | "ERR_OSSL_ASN1_NO_MULTIPART_BODY_FAILURE"
    | "ERR_OSSL_ASN1_NO_MULTIPART_BOUNDARY" | "ERR_OSSL_ASN1_NO_SIG_CONTENT_TYPE"
    | "ERR_OSSL_ASN1_NULL_IS_WRONG_LENGTH" | "ERR_OSSL_ASN1_OBJECT_NOT_ASCII_FORMAT"
    | "ERR_OSSL_ASN1_ODD_NUMBER_OF_CHARS" | "ERR_OSSL_ASN1_SECOND_NUMBER_TOO_LARGE"
    | "ERR_OSSL_ASN1_SEQUENCE_LENGTH_MISMATCH" | "ERR_OSSL_ASN1_SEQUENCE_NOT_CONSTRUCTED"
    | "ERR_OSSL_ASN1_SEQUENCE_OR_SET_NEEDS_CONFIG" | "ERR_OSSL_ASN1_SHORT_LINE"
    | "ERR_OSSL_ASN1_SIG_INVALID_MIME_TYPE" | "ERR_OSSL_ASN1_STREAMING_NOT_SUPPORTED"
    | "ERR_OSSL_ASN1_STRING_TOO_LONG" | "ERR_OSSL_ASN1_STRING_TOO_SHORT"
    | "ERR_OSSL_ASN1_THE_ASN1_OBJECT_IDENTIFIER_IS_NOT_KNOWN_FOR_THIS_MD"
    | "ERR_OSSL_ASN1_TIME_NOT_ASCII_FORMAT" | "ERR_OSSL_ASN1_TOO_LARGE" | "ERR_OSSL_ASN1_TOO_LONG"
    | "ERR_OSSL_ASN1_TOO_SMALL" | "ERR_OSSL_ASN1_TYPE_NOT_CONSTRUCTED"
    | "ERR_OSSL_ASN1_TYPE_NOT_PRIMITIVE" | "ERR_OSSL_ASN1_UNEXPECTED_EOC"
    | "ERR_OSSL_ASN1_UNIVERSALSTRING_IS_WRONG_LENGTH" | "ERR_OSSL_ASN1_UNKNOWN_DIGEST"
    | "ERR_OSSL_ASN1_UNKNOWN_FORMAT" | "ERR_OSSL_ASN1_UNKNOWN_MESSAGE_DIGEST_ALGORITHM"
    | "ERR_OSSL_ASN1_UNKNOWN_OBJECT_TYPE" | "ERR_OSSL_ASN1_UNKNOWN_PUBLIC_KEY_TYPE"
    | "ERR_OSSL_ASN1_UNKNOWN_SIGNATURE_ALGORITHM" | "ERR_OSSL_ASN1_UNKNOWN_TAG"
    | "ERR_OSSL_ASN1_UNSUPPORTED_ANY_DEFINED_BY_TYPE" | "ERR_OSSL_ASN1_UNSUPPORTED_CIPHER"
    | "ERR_OSSL_ASN1_UNSUPPORTED_PUBLIC_KEY_TYPE" | "ERR_OSSL_ASN1_UNSUPPORTED_TYPE"
    | "ERR_OSSL_ASN1_UTCTIME_IS_TOO_SHORT" | "ERR_OSSL_ASN1_WRONG_INTEGER_TYPE"
    | "ERR_OSSL_ASN1_WRONG_PUBLIC_KEY_TYPE" | "ERR_OSSL_ASN1_WRONG_TAG" | "ERR_OSSL_EC_ASN1_ERROR"
    | "ERR_OSSL_EC_BAD_SIGNATURE" | "ERR_OSSL_EC_BIGNUM_OUT_OF_RANGE" | "ERR_OSSL_EC_BUFFER_TOO_SMALL"
    | "ERR_OSSL_EC_CANNOT_INVERT" | "ERR_OSSL_EC_COORDINATES_OUT_OF_RANGE"
    | "ERR_OSSL_EC_CURVE_DOES_NOT_SUPPORT_ECDH" | "ERR_OSSL_EC_CURVE_DOES_NOT_SUPPORT_ECDSA"
    | "ERR_OSSL_EC_CURVE_DOES_NOT_SUPPORT_SIGNING" | "ERR_OSSL_EC_DECODE_ERROR"
    | "ERR_OSSL_EC_DISCRIMINANT_IS_ZERO" | "ERR_OSSL_EC_EC_GROUP_NEW_BY_NAME_FAILURE"
    | "ERR_OSSL_EC_EXPLICIT_PARAMS_NOT_SUPPORTED" | "ERR_OSSL_EC_FAILED_MAKING_PUBLIC_KEY"
    | "ERR_OSSL_EC_FIELD_TOO_LARGE" | "ERR_OSSL_EC_GF2M_NOT_SUPPORTED"
    | "ERR_OSSL_EC_GROUP2PKPARAMETERS_FAILURE" | "ERR_OSSL_EC_I2D_ECPKPARAMETERS_FAILURE"
    | "ERR_OSSL_EC_INCOMPATIBLE_OBJECTS" | "ERR_OSSL_EC_INVALID_A" | "ERR_OSSL_EC_INVALID_ARGUMENT"
    | "ERR_OSSL_EC_INVALID_B" | "ERR_OSSL_EC_INVALID_COFACTOR"
    | "ERR_OSSL_EC_INVALID_COMPRESSED_POINT" | "ERR_OSSL_EC_INVALID_COMPRESSION_BIT"
    | "ERR_OSSL_EC_INVALID_CURVE" | "ERR_OSSL_EC_INVALID_DIGEST" | "ERR_OSSL_EC_INVALID_DIGEST_TYPE"
    | "ERR_OSSL_EC_INVALID_ENCODING" | "ERR_OSSL_EC_INVALID_FIELD" | "ERR_OSSL_EC_INVALID_FORM"
    | "ERR_OSSL_EC_INVALID_GENERATOR" | "ERR_OSSL_EC_INVALID_GROUP_ORDER" | "ERR_OSSL_EC_INVALID_KEY"
    | "ERR_OSSL_EC_INVALID_LENGTH" | "ERR_OSSL_EC_INVALID_NAMED_GROUP_CONVERSION"
    | "ERR_OSSL_EC_INVALID_OUTPUT_LENGTH" | "ERR_OSSL_EC_INVALID_P" | "ERR_OSSL_EC_INVALID_PEER_KEY"
    | "ERR_OSSL_EC_INVALID_PENTANOMIAL_BASIS" | "ERR_OSSL_EC_INVALID_PRIVATE_KEY"
    | "ERR_OSSL_EC_INVALID_SEED" | "ERR_OSSL_EC_INVALID_TRINOMIAL_BASIS"
    | "ERR_OSSL_EC_KDF_PARAMETER_ERROR" | "ERR_OSSL_EC_KEYS_NOT_SET"
    | "ERR_OSSL_EC_LADDER_POST_FAILURE" | "ERR_OSSL_EC_LADDER_PRE_FAILURE"
    | "ERR_OSSL_EC_LADDER_STEP_FAILURE" | "ERR_OSSL_EC_MISSING_OID" | "ERR_OSSL_EC_MISSING_PARAMETERS"
    | "ERR_OSSL_EC_MISSING_PRIVATE_KEY" | "ERR_OSSL_EC_NEED_NEW_SETUP_VALUES"
    | "ERR_OSSL_EC_NOT_A_NIST_PRIME" | "ERR_OSSL_EC_NOT_IMPLEMENTED" | "ERR_OSSL_EC_NOT_INITIALIZED"
    | "ERR_OSSL_EC_NO_PARAMETERS_SET" | "ERR_OSSL_EC_NO_PRIVATE_VALUE"
    | "ERR_OSSL_EC_OPERATION_NOT_SUPPORTED" | "ERR_OSSL_EC_PASSED_NULL_PARAMETER"
    | "ERR_OSSL_EC_PEER_KEY_ERROR" | "ERR_OSSL_EC_POINT_ARITHMETIC_FAILURE"
    | "ERR_OSSL_EC_POINT_AT_INFINITY" | "ERR_OSSL_EC_POINT_COORDINATES_BLIND_FAILURE"
    | "ERR_OSSL_EC_POINT_IS_NOT_ON_CURVE" | "ERR_OSSL_EC_RANDOM_NUMBER_GENERATION_FAILED"
    | "ERR_OSSL_EC_SHARED_INFO_ERROR" | "ERR_OSSL_EC_SLOT_FULL" | "ERR_OSSL_EC_TOO_MANY_RETRIES"
    | "ERR_OSSL_EC_UNDEFINED_GENERATOR" | "ERR_OSSL_EC_UNDEFINED_ORDER"
    | "ERR_OSSL_EC_UNKNOWN_COFACTOR" | "ERR_OSSL_EC_UNKNOWN_GROUP" | "ERR_OSSL_EC_UNKNOWN_ORDER"
    | "ERR_OSSL_EC_UNSUPPORTED_FIELD" | "ERR_OSSL_EC_WRONG_CURVE_PARAMETERS"
    | "ERR_OSSL_EC_WRONG_ORDER" | "ERR_OSSL_RSA_ALGORITHM_MISMATCH" | "ERR_OSSL_RSA_BAD_E_VALUE"
    | "ERR_OSSL_RSA_BAD_FIXED_HEADER_DECRYPT" | "ERR_OSSL_RSA_BAD_PAD_BYTE_COUNT"
    | "ERR_OSSL_RSA_BAD_SIGNATURE" | "ERR_OSSL_RSA_BLOCK_TYPE_IS_NOT_01"
    | "ERR_OSSL_RSA_BLOCK_TYPE_IS_NOT_02" | "ERR_OSSL_RSA_DATA_GREATER_THAN_MOD_LEN"
    | "ERR_OSSL_RSA_DATA_TOO_LARGE" | "ERR_OSSL_RSA_DATA_TOO_LARGE_FOR_KEY_SIZE"
    | "ERR_OSSL_RSA_DATA_TOO_LARGE_FOR_MODULUS" | "ERR_OSSL_RSA_DATA_TOO_SMALL"
    | "ERR_OSSL_RSA_DATA_TOO_SMALL_FOR_KEY_SIZE" | "ERR_OSSL_RSA_DIGEST_DOES_NOT_MATCH"
    | "ERR_OSSL_RSA_DIGEST_NOT_ALLOWED" | "ERR_OSSL_RSA_DIGEST_TOO_BIG_FOR_RSA_KEY"
    | "ERR_OSSL_RSA_DMP1_NOT_CONGRUENT_TO_D" | "ERR_OSSL_RSA_DMQ1_NOT_CONGRUENT_TO_D"
    | "ERR_OSSL_RSA_D_E_NOT_CONGRUENT_TO_1" | "ERR_OSSL_RSA_FIRST_OCTET_INVALID"
    | "ERR_OSSL_RSA_ILLEGAL_OR_UNSUPPORTED_PADDING_MODE" | "ERR_OSSL_RSA_INVALID_DIGEST"
    | "ERR_OSSL_RSA_INVALID_DIGEST_LENGTH" | "ERR_OSSL_RSA_INVALID_HEADER"
    | "ERR_OSSL_RSA_INVALID_KEYPAIR" | "ERR_OSSL_RSA_INVALID_KEY_LENGTH"
    | "ERR_OSSL_RSA_INVALID_LABEL" | "ERR_OSSL_RSA_INVALID_LENGTH"
    | "ERR_OSSL_RSA_INVALID_MESSAGE_LENGTH" | "ERR_OSSL_RSA_INVALID_MGF1_MD"
    | "ERR_OSSL_RSA_INVALID_MODULUS" | "ERR_OSSL_RSA_INVALID_MULTI_PRIME_KEY"
    | "ERR_OSSL_RSA_INVALID_OAEP_PARAMETERS" | "ERR_OSSL_RSA_INVALID_PADDING"
    | "ERR_OSSL_RSA_INVALID_PADDING_MODE" | "ERR_OSSL_RSA_INVALID_PSS_PARAMETERS"
    | "ERR_OSSL_RSA_INVALID_PSS_SALTLEN" | "ERR_OSSL_RSA_INVALID_REQUEST"
    | "ERR_OSSL_RSA_INVALID_SALT_LENGTH" | "ERR_OSSL_RSA_INVALID_STRENGTH"
    | "ERR_OSSL_RSA_INVALID_TRAILER" | "ERR_OSSL_RSA_INVALID_X931_DIGEST"
    | "ERR_OSSL_RSA_IQMP_NOT_INVERSE_OF_Q" | "ERR_OSSL_RSA_KEY_PRIME_NUM_INVALID"
    | "ERR_OSSL_RSA_KEY_SIZE_TOO_SMALL" | "ERR_OSSL_RSA_LAST_OCTET_INVALID"
    | "ERR_OSSL_RSA_MGF1_DIGEST_NOT_ALLOWED" | "ERR_OSSL_RSA_MISSING_PRIVATE_KEY"
    | "ERR_OSSL_RSA_MODULUS_TOO_LARGE" | "ERR_OSSL_RSA_MP_COEFFICIENT_NOT_INVERSE_OF_R"
    | "ERR_OSSL_RSA_MP_EXPONENT_NOT_CONGRUENT_TO_D" | "ERR_OSSL_RSA_MP_R_NOT_PRIME"
    | "ERR_OSSL_RSA_NO_PUBLIC_EXPONENT" | "ERR_OSSL_RSA_NULL_BEFORE_BLOCK_MISSING"
    | "ERR_OSSL_RSA_N_DOES_NOT_EQUAL_PRODUCT_OF_PRIMES" | "ERR_OSSL_RSA_N_DOES_NOT_EQUAL_P_Q"
    | "ERR_OSSL_RSA_OAEP_DECODING_ERROR" | "ERR_OSSL_RSA_OPERATION_NOT_SUPPORTED_FOR_THIS_KEYTYPE"
    | "ERR_OSSL_RSA_PADDING_CHECK_FAILED" | "ERR_OSSL_RSA_PAIRWISE_TEST_FAILURE"
    | "ERR_OSSL_RSA_PKCS_DECODING_ERROR" | "ERR_OSSL_RSA_PSS_SALTLEN_TOO_SMALL"
    | "ERR_OSSL_RSA_PUB_EXPONENT_OUT_OF_RANGE" | "ERR_OSSL_RSA_P_NOT_PRIME"
    | "ERR_OSSL_RSA_Q_NOT_PRIME" | "ERR_OSSL_RSA_RANDOMNESS_SOURCE_STRENGTH_INSUFFICIENT"
    | "ERR_OSSL_RSA_RSA_OPERATIONS_NOT_SUPPORTED" | "ERR_OSSL_RSA_SLEN_CHECK_FAILED"
    | "ERR_OSSL_RSA_SLEN_RECOVERY_FAILED" | "ERR_OSSL_RSA_SSLV3_ROLLBACK_ATTACK"
    | "ERR_OSSL_RSA_THE_ASN1_OBJECT_IDENTIFIER_IS_NOT_KNOWN_FOR_THIS_MD"
    | "ERR_OSSL_RSA_UNKNOWN_ALGORITHM_TYPE" | "ERR_OSSL_RSA_UNKNOWN_DIGEST"
    | "ERR_OSSL_RSA_UNKNOWN_MASK_DIGEST" | "ERR_OSSL_RSA_UNKNOWN_PADDING_TYPE"
    | "ERR_OSSL_RSA_UNSUPPORTED_ENCRYPTION_TYPE" | "ERR_OSSL_RSA_UNSUPPORTED_LABEL_SOURCE"
    | "ERR_OSSL_RSA_UNSUPPORTED_MASK_ALGORITHM" | "ERR_OSSL_RSA_UNSUPPORTED_MASK_PARAMETER"
    | "ERR_OSSL_RSA_UNSUPPORTED_SIGNATURE_TYPE" | "ERR_OSSL_RSA_VALUE_MISSING"
    | "ERR_OSSL_RSA_WRONG_SIGNATURE_LENGTH" | "ERR_OSSL_EVP_AES_KEY_SETUP_FAILED"
    | "ERR_OSSL_EVP_ARIA_KEY_SETUP_FAILED" | "ERR_OSSL_EVP_BAD_ALGORITHM_NAME"
    | "ERR_OSSL_EVP_BAD_DECRYPT" | "ERR_OSSL_EVP_BAD_KEY_LENGTH" | "ERR_OSSL_EVP_BUFFER_TOO_SMALL"
    | "ERR_OSSL_EVP_CACHE_CONSTANTS_FAILED" | "ERR_OSSL_EVP_CAMELLIA_KEY_SETUP_FAILED"
    | "ERR_OSSL_EVP_CANNOT_GET_PARAMETERS" | "ERR_OSSL_EVP_CANNOT_SET_PARAMETERS"
    | "ERR_OSSL_EVP_CIPHER_NOT_GCM_MODE" | "ERR_OSSL_EVP_CIPHER_PARAMETER_ERROR"
    | "ERR_OSSL_EVP_COMMAND_NOT_SUPPORTED" | "ERR_OSSL_EVP_CONFLICTING_ALGORITHM_NAME"
    | "ERR_OSSL_EVP_COPY_ERROR" | "ERR_OSSL_EVP_CTRL_NOT_IMPLEMENTED"
    | "ERR_OSSL_EVP_CTRL_OPERATION_NOT_IMPLEMENTED" | "ERR_OSSL_EVP_DATA_NOT_MULTIPLE_OF_BLOCK_LENGTH"
    | "ERR_OSSL_EVP_DECODE_ERROR" | "ERR_OSSL_EVP_DEFAULT_QUERY_PARSE_ERROR"
    | "ERR_OSSL_EVP_DIFFERENT_KEY_TYPES" | "ERR_OSSL_EVP_DIFFERENT_PARAMETERS"
    | "ERR_OSSL_EVP_ERROR_LOADING_SECTION" | "ERR_OSSL_EVP_EXPECTING_AN_HMAC_KEY"
    | "ERR_OSSL_EVP_EXPECTING_AN_RSA_KEY" | "ERR_OSSL_EVP_EXPECTING_A_DH_KEY"
    | "ERR_OSSL_EVP_EXPECTING_A_DSA_KEY" | "ERR_OSSL_EVP_EXPECTING_A_ECX_KEY"
    | "ERR_OSSL_EVP_EXPECTING_A_EC_KEY" | "ERR_OSSL_EVP_EXPECTING_A_POLY1305_KEY"
    | "ERR_OSSL_EVP_EXPECTING_A_SIPHASH_KEY" | "ERR_OSSL_EVP_FINAL_ERROR"
    | "ERR_OSSL_EVP_GENERATE_ERROR" | "ERR_OSSL_EVP_GETTING_ALGORITHMIDENTIFIER_NOT_SUPPORTED"
    | "ERR_OSSL_EVP_GET_RAW_KEY_FAILED" | "ERR_OSSL_EVP_ILLEGAL_SCRYPT_PARAMETERS"
    | "ERR_OSSL_EVP_INACCESSIBLE_DOMAIN_PARAMETERS" | "ERR_OSSL_EVP_INACCESSIBLE_KEY"
    | "ERR_OSSL_EVP_INITIALIZATION_ERROR" | "ERR_OSSL_EVP_INPUT_NOT_INITIALIZED"
    | "ERR_OSSL_EVP_INVALID_CUSTOM_LENGTH" | "ERR_OSSL_EVP_INVALID_IV_LENGTH"
    | "ERR_OSSL_EVP_INVALID_KEY" | "ERR_OSSL_EVP_INVALID_KEY_LENGTH" | "ERR_OSSL_EVP_INVALID_LENGTH"
    | "ERR_OSSL_EVP_INVALID_NULL_ALGORITHM" | "ERR_OSSL_EVP_INVALID_OPERATION"
    | "ERR_OSSL_EVP_INVALID_PROVIDER_FUNCTIONS" | "ERR_OSSL_EVP_INVALID_SALT_LENGTH"
    | "ERR_OSSL_EVP_INVALID_SECRET_LENGTH" | "ERR_OSSL_EVP_INVALID_SEED_LENGTH"
    | "ERR_OSSL_EVP_INVALID_VALUE" | "ERR_OSSL_EVP_KEYMGMT_EXPORT_FAILURE"
    | "ERR_OSSL_EVP_KEY_SETUP_FAILED" | "ERR_OSSL_EVP_LOCKING_NOT_SUPPORTED"
    | "ERR_OSSL_EVP_MEMORY_LIMIT_EXCEEDED" | "ERR_OSSL_EVP_MESSAGE_DIGEST_IS_NULL"
    | "ERR_OSSL_EVP_METHOD_NOT_SUPPORTED" | "ERR_OSSL_EVP_MISSING_PARAMETERS"
    | "ERR_OSSL_EVP_NOT_ABLE_TO_COPY_CTX" | "ERR_OSSL_EVP_NOT_XOF_OR_INVALID_LENGTH"
    | "ERR_OSSL_EVP_NO_CIPHER_SET" | "ERR_OSSL_EVP_NO_DEFAULT_DIGEST" | "ERR_OSSL_EVP_NO_DIGEST_SET"
    | "ERR_OSSL_EVP_NO_IMPORT_FUNCTION" | "ERR_OSSL_EVP_NO_KEYMGMT_AVAILABLE"
    | "ERR_OSSL_EVP_NO_KEYMGMT_PRESENT" | "ERR_OSSL_EVP_NO_KEY_SET" | "ERR_OSSL_EVP_NO_OPERATION_SET"
    | "ERR_OSSL_EVP_NULL_MAC_PKEY_CTX" | "ERR_OSSL_EVP_ONLY_ONESHOT_SUPPORTED"
    | "ERR_OSSL_EVP_OPERATION_NOT_INITIALIZED"
    | "ERR_OSSL_EVP_OPERATION_NOT_SUPPORTED_FOR_THIS_KEYTYPE"
    | "ERR_OSSL_EVP_OPERATION_NOT_SUPPORTED_FOR_THIS_SIGNATURE_TYPE"
    | "ERR_OSSL_EVP_OUTPUT_WOULD_OVERFLOW" | "ERR_OSSL_EVP_PARAMETER_TOO_LARGE"
    | "ERR_OSSL_EVP_PARTIALLY_OVERLAPPING" | "ERR_OSSL_EVP_PBKDF2_ERROR"
    | "ERR_OSSL_EVP_PIPELINE_NOT_SUPPORTED"
    | "ERR_OSSL_EVP_PKEY_APPLICATION_ASN1_METHOD_ALREADY_REGISTERED"
    | "ERR_OSSL_EVP_PRIVATE_KEY_DECODE_ERROR" | "ERR_OSSL_EVP_PRIVATE_KEY_ENCODE_ERROR"
    | "ERR_OSSL_EVP_PROVIDER_ASYM_CIPHER_FAILURE" | "ERR_OSSL_EVP_PROVIDER_ASYM_CIPHER_NOT_SUPPORTED"
    | "ERR_OSSL_EVP_PROVIDER_KEYMGMT_FAILURE" | "ERR_OSSL_EVP_PROVIDER_KEYMGMT_NOT_SUPPORTED"
    | "ERR_OSSL_EVP_PROVIDER_SIGNATURE_FAILURE" | "ERR_OSSL_EVP_PROVIDER_SIGNATURE_NOT_SUPPORTED"
    | "ERR_OSSL_EVP_PUBLIC_KEY_NOT_RSA" | "ERR_OSSL_EVP_SETTING_XOF_FAILED"
    | "ERR_OSSL_EVP_SET_DEFAULT_PROPERTY_FAILURE"
    | "ERR_OSSL_EVP_SIGNATURE_TYPE_AND_KEY_TYPE_INCOMPATIBLE" | "ERR_OSSL_EVP_TOO_MANY_PIPES"
    | "ERR_OSSL_EVP_TOO_MANY_RECORDS" | "ERR_OSSL_EVP_UNABLE_TO_ENABLE_LOCKING"
    | "ERR_OSSL_EVP_UNABLE_TO_GET_MAXIMUM_REQUEST_SIZE" | "ERR_OSSL_EVP_UNABLE_TO_GET_RANDOM_STRENGTH"
    | "ERR_OSSL_EVP_UNABLE_TO_LOCK_CONTEXT" | "ERR_OSSL_EVP_UNABLE_TO_SET_CALLBACKS"
    | "ERR_OSSL_EVP_UNKNOWN_BITS" | "ERR_OSSL_EVP_UNKNOWN_CIPHER" | "ERR_OSSL_EVP_UNKNOWN_DIGEST"
    | "ERR_OSSL_EVP_UNKNOWN_KEY_TYPE" | "ERR_OSSL_EVP_UNKNOWN_MAX_SIZE"
    | "ERR_OSSL_EVP_UNKNOWN_OPTION" | "ERR_OSSL_EVP_UNKNOWN_PBE_ALGORITHM"
    | "ERR_OSSL_EVP_UNKNOWN_SECURITY_BITS" | "ERR_OSSL_EVP_UNSUPPORTED_ALGORITHM"
    | "ERR_OSSL_EVP_UNSUPPORTED_CIPHER" | "ERR_OSSL_EVP_UNSUPPORTED_KEYLENGTH"
    | "ERR_OSSL_EVP_UNSUPPORTED_KEY_DERIVATION_FUNCTION" | "ERR_OSSL_EVP_UNSUPPORTED_KEY_SIZE"
    | "ERR_OSSL_EVP_UNSUPPORTED_KEY_TYPE" | "ERR_OSSL_EVP_UNSUPPORTED_NUMBER_OF_ROUNDS"
    | "ERR_OSSL_EVP_UNSUPPORTED_PRF" | "ERR_OSSL_EVP_UNSUPPORTED_PRIVATE_KEY_ALGORITHM"
    | "ERR_OSSL_EVP_UNSUPPORTED_SALT_TYPE" | "ERR_OSSL_EVP_UPDATE_ERROR"
    | "ERR_OSSL_EVP_WRAP_MODE_NOT_ALLOWED" | "ERR_OSSL_EVP_WRONG_FINAL_BLOCK_LENGTH"
    | "ERR_OSSL_EVP_XTS_DATA_UNIT_IS_TOO_LARGE" | "ERR_OSSL_EVP_XTS_DUPLICATED_KEYS"
  type CertificateReadCode =
    | "ERR_OSSL_PEM_BAD_BASE64_DECODE" | "ERR_OSSL_PEM_BAD_DECRYPT" | "ERR_OSSL_PEM_BAD_END_LINE"
    | "ERR_OSSL_PEM_BAD_IV_CHARS" | "ERR_OSSL_PEM_BAD_MAGIC_NUMBER" | "ERR_OSSL_PEM_BAD_PASSWORD_READ"
    | "ERR_OSSL_PEM_BAD_VERSION_NUMBER" | "ERR_OSSL_PEM_BIO_WRITE_FAILURE"
    | "ERR_OSSL_PEM_CIPHER_IS_NULL" | "ERR_OSSL_PEM_ERROR_CONVERTING_PRIVATE_KEY"
    | "ERR_OSSL_PEM_EXPECTING_DSS_KEY_BLOB" | "ERR_OSSL_PEM_EXPECTING_PRIVATE_KEY_BLOB"
    | "ERR_OSSL_PEM_EXPECTING_PUBLIC_KEY_BLOB" | "ERR_OSSL_PEM_EXPECTING_RSA_KEY_BLOB"
    | "ERR_OSSL_PEM_HEADER_TOO_LONG" | "ERR_OSSL_PEM_INCONSISTENT_HEADER"
    | "ERR_OSSL_PEM_KEYBLOB_HEADER_PARSE_ERROR" | "ERR_OSSL_PEM_KEYBLOB_TOO_SHORT"
    | "ERR_OSSL_PEM_MISSING_DEK_IV" | "ERR_OSSL_PEM_NOT_DEK_INFO" | "ERR_OSSL_PEM_NOT_ENCRYPTED"
    | "ERR_OSSL_PEM_NOT_PROC_TYPE" | "ERR_OSSL_PEM_NO_START_LINE"
    | "ERR_OSSL_PEM_PROBLEMS_GETTING_PASSWORD" | "ERR_OSSL_PEM_PVK_DATA_TOO_SHORT"
    | "ERR_OSSL_PEM_PVK_TOO_SHORT" | "ERR_OSSL_PEM_READ_KEY" | "ERR_OSSL_PEM_SHORT_HEADER"
    | "ERR_OSSL_PEM_UNEXPECTED_DEK_IV" | "ERR_OSSL_PEM_UNSUPPORTED_CIPHER"
    | "ERR_OSSL_PEM_UNSUPPORTED_ENCRYPTION" | "ERR_OSSL_PEM_UNSUPPORTED_KEY_COMPONENTS"
    | "ERR_OSSL_PEM_UNSUPPORTED_PUBLIC_KEY_TYPE" | "ERR_OSSL_PEM_UNSUPPORTED_PVK_KEY_TYPE"
    | "ERR_OSSL_ASN1_ADDING_OBJECT" | "ERR_OSSL_ASN1_ASN1_PARSE_ERROR"
    | "ERR_OSSL_ASN1_ASN1_SIG_PARSE_ERROR" | "ERR_OSSL_ASN1_AUX_ERROR"
    | "ERR_OSSL_ASN1_BAD_OBJECT_HEADER" | "ERR_OSSL_ASN1_BAD_TEMPLATE"
    | "ERR_OSSL_ASN1_BMPSTRING_IS_WRONG_LENGTH" | "ERR_OSSL_ASN1_BN_LIB"
    | "ERR_OSSL_ASN1_BOOLEAN_IS_WRONG_LENGTH" | "ERR_OSSL_ASN1_BUFFER_TOO_SMALL"
    | "ERR_OSSL_ASN1_CIPHER_HAS_NO_OBJECT_IDENTIFIER" | "ERR_OSSL_ASN1_CONTEXT_NOT_INITIALISED"
    | "ERR_OSSL_ASN1_DATA_IS_WRONG" | "ERR_OSSL_ASN1_DECODE_ERROR" | "ERR_OSSL_ASN1_DEPTH_EXCEEDED"
    | "ERR_OSSL_ASN1_DIGEST_AND_KEY_TYPE_NOT_SUPPORTED" | "ERR_OSSL_ASN1_ENCODE_ERROR"
    | "ERR_OSSL_ASN1_ERROR_GETTING_TIME" | "ERR_OSSL_ASN1_ERROR_LOADING_SECTION"
    | "ERR_OSSL_ASN1_ERROR_SETTING_CIPHER_PARAMS" | "ERR_OSSL_ASN1_EXPECTING_AN_INTEGER"
    | "ERR_OSSL_ASN1_EXPECTING_AN_OBJECT" | "ERR_OSSL_ASN1_EXPLICIT_LENGTH_MISMATCH"
    | "ERR_OSSL_ASN1_EXPLICIT_TAG_NOT_CONSTRUCTED" | "ERR_OSSL_ASN1_FIELD_MISSING"
    | "ERR_OSSL_ASN1_FIRST_NUM_TOO_LARGE" | "ERR_OSSL_ASN1_GENERALIZEDTIME_IS_TOO_SHORT"
    | "ERR_OSSL_ASN1_HEADER_TOO_LONG" | "ERR_OSSL_ASN1_ILLEGAL_BITSTRING_FORMAT"
    | "ERR_OSSL_ASN1_ILLEGAL_BOOLEAN" | "ERR_OSSL_ASN1_ILLEGAL_CHARACTERS"
    | "ERR_OSSL_ASN1_ILLEGAL_FORMAT" | "ERR_OSSL_ASN1_ILLEGAL_HEX"
    | "ERR_OSSL_ASN1_ILLEGAL_IMPLICIT_TAG" | "ERR_OSSL_ASN1_ILLEGAL_INTEGER"
    | "ERR_OSSL_ASN1_ILLEGAL_NEGATIVE_VALUE" | "ERR_OSSL_ASN1_ILLEGAL_NESTED_TAGGING"
    | "ERR_OSSL_ASN1_ILLEGAL_NULL" | "ERR_OSSL_ASN1_ILLEGAL_NULL_VALUE"
    | "ERR_OSSL_ASN1_ILLEGAL_OBJECT" | "ERR_OSSL_ASN1_ILLEGAL_OPTIONAL_ANY"
    | "ERR_OSSL_ASN1_ILLEGAL_OPTIONS_ON_ITEM_TEMPLATE" | "ERR_OSSL_ASN1_ILLEGAL_PADDING"
    | "ERR_OSSL_ASN1_ILLEGAL_TAGGED_ANY" | "ERR_OSSL_ASN1_ILLEGAL_TIME_VALUE"
    | "ERR_OSSL_ASN1_ILLEGAL_ZERO_CONTENT" | "ERR_OSSL_ASN1_INTEGER_NOT_ASCII_FORMAT"
    | "ERR_OSSL_ASN1_INTEGER_TOO_LARGE_FOR_LONG" | "ERR_OSSL_ASN1_INVALID_BIT_STRING_BITS_LEFT"
    | "ERR_OSSL_ASN1_INVALID_BMPSTRING_LENGTH" | "ERR_OSSL_ASN1_INVALID_DIGIT"
    | "ERR_OSSL_ASN1_INVALID_MIME_TYPE" | "ERR_OSSL_ASN1_INVALID_MODIFIER"
    | "ERR_OSSL_ASN1_INVALID_NUMBER" | "ERR_OSSL_ASN1_INVALID_OBJECT_ENCODING"
    | "ERR_OSSL_ASN1_INVALID_SCRYPT_PARAMETERS" | "ERR_OSSL_ASN1_INVALID_SEPARATOR"
    | "ERR_OSSL_ASN1_INVALID_STRING_TABLE_VALUE" | "ERR_OSSL_ASN1_INVALID_UNIVERSALSTRING_LENGTH"
    | "ERR_OSSL_ASN1_INVALID_UTF8STRING" | "ERR_OSSL_ASN1_INVALID_VALUE"
    | "ERR_OSSL_ASN1_LENGTH_TOO_LONG" | "ERR_OSSL_ASN1_LIST_ERROR"
    | "ERR_OSSL_ASN1_MIME_NO_CONTENT_TYPE" | "ERR_OSSL_ASN1_MIME_PARSE_ERROR"
    | "ERR_OSSL_ASN1_MIME_SIG_PARSE_ERROR" | "ERR_OSSL_ASN1_MISSING_EOC"
    | "ERR_OSSL_ASN1_MISSING_SECOND_NUMBER" | "ERR_OSSL_ASN1_MISSING_VALUE"
    | "ERR_OSSL_ASN1_MSTRING_NOT_UNIVERSAL" | "ERR_OSSL_ASN1_MSTRING_WRONG_TAG"
    | "ERR_OSSL_ASN1_NESTED_ASN1_STRING" | "ERR_OSSL_ASN1_NESTED_TOO_DEEP"
    | "ERR_OSSL_ASN1_NON_HEX_CHARACTERS" | "ERR_OSSL_ASN1_NOT_ASCII_FORMAT"
    | "ERR_OSSL_ASN1_NOT_ENOUGH_DATA" | "ERR_OSSL_ASN1_NO_CONTENT_TYPE"
    | "ERR_OSSL_ASN1_NO_MATCHING_CHOICE_TYPE" | "ERR_OSSL_ASN1_NO_MULTIPART_BODY_FAILURE"
    | "ERR_OSSL_ASN1_NO_MULTIPART_BOUNDARY" | "ERR_OSSL_ASN1_NO_SIG_CONTENT_TYPE"
    | "ERR_OSSL_ASN1_NULL_IS_WRONG_LENGTH" | "ERR_OSSL_ASN1_OBJECT_NOT_ASCII_FORMAT"
    | "ERR_OSSL_ASN1_ODD_NUMBER_OF_CHARS" | "ERR_OSSL_ASN1_SECOND_NUMBER_TOO_LARGE"
    | "ERR_OSSL_ASN1_SEQUENCE_LENGTH_MISMATCH" | "ERR_OSSL_ASN1_SEQUENCE_NOT_CONSTRUCTED"
    | "ERR_OSSL_ASN1_SEQUENCE_OR_SET_NEEDS_CONFIG" | "ERR_OSSL_ASN1_SHORT_LINE"
    | "ERR_OSSL_ASN1_SIG_INVALID_MIME_TYPE" | "ERR_OSSL_ASN1_STREAMING_NOT_SUPPORTED"
    | "ERR_OSSL_ASN1_STRING_TOO_LONG" | "ERR_OSSL_ASN1_STRING_TOO_SHORT"
    | "ERR_OSSL_ASN1_THE_ASN1_OBJECT_IDENTIFIER_IS_NOT_KNOWN_FOR_THIS_MD"
    | "ERR_OSSL_ASN1_TIME_NOT_ASCII_FORMAT" | "ERR_OSSL_ASN1_TOO_LARGE" | "ERR_OSSL_ASN1_TOO_LONG"
    | "ERR_OSSL_ASN1_TOO_SMALL" | "ERR_OSSL_ASN1_TYPE_NOT_CONSTRUCTED"
    | "ERR_OSSL_ASN1_TYPE_NOT_PRIMITIVE" | "ERR_OSSL_ASN1_UNEXPECTED_EOC"
    | "ERR_OSSL_ASN1_UNIVERSALSTRING_IS_WRONG_LENGTH" | "ERR_OSSL_ASN1_UNKNOWN_DIGEST"
    | "ERR_OSSL_ASN1_UNKNOWN_FORMAT" | "ERR_OSSL_ASN1_UNKNOWN_MESSAGE_DIGEST_ALGORITHM"
    | "ERR_OSSL_ASN1_UNKNOWN_OBJECT_TYPE" | "ERR_OSSL_ASN1_UNKNOWN_PUBLIC_KEY_TYPE"
    | "ERR_OSSL_ASN1_UNKNOWN_SIGNATURE_ALGORITHM" | "ERR_OSSL_ASN1_UNKNOWN_TAG"
    | "ERR_OSSL_ASN1_UNSUPPORTED_ANY_DEFINED_BY_TYPE" | "ERR_OSSL_ASN1_UNSUPPORTED_CIPHER"
    | "ERR_OSSL_ASN1_UNSUPPORTED_PUBLIC_KEY_TYPE" | "ERR_OSSL_ASN1_UNSUPPORTED_TYPE"
    | "ERR_OSSL_ASN1_UTCTIME_IS_TOO_SHORT" | "ERR_OSSL_ASN1_WRONG_INTEGER_TYPE"
    | "ERR_OSSL_ASN1_WRONG_PUBLIC_KEY_TYPE" | "ERR_OSSL_ASN1_WRONG_TAG"
    | "ERR_OSSL_X509_AKID_MISMATCH" | "ERR_OSSL_X509_BAD_SELECTOR" | "ERR_OSSL_X509_BAD_X509_FILETYPE"
    | "ERR_OSSL_X509_BASE64_DECODE_ERROR" | "ERR_OSSL_X509_CANT_CHECK_DH_KEY"
    | "ERR_OSSL_X509_CERTIFICATE_VERIFICATION_FAILED" | "ERR_OSSL_X509_CERT_ALREADY_IN_HASH_TABLE"
    | "ERR_OSSL_X509_CRL_ALREADY_DELTA" | "ERR_OSSL_X509_CRL_SIGNATURE_ALGORITHM_MISMATCH"
    | "ERR_OSSL_X509_CRL_VERIFY_FAILURE" | "ERR_OSSL_X509_DUPLICATE_ATTRIBUTE"
    | "ERR_OSSL_X509_ERROR_GETTING_MD_BY_NID" | "ERR_OSSL_X509_ERROR_USING_SIGINF_SET"
    | "ERR_OSSL_X509_IDP_MISMATCH" | "ERR_OSSL_X509_INVALID_ATTRIBUTES"
    | "ERR_OSSL_X509_INVALID_DIRECTORY" | "ERR_OSSL_X509_INVALID_DISTPOINT"
    | "ERR_OSSL_X509_INVALID_FIELD_NAME" | "ERR_OSSL_X509_INVALID_TRUST"
    | "ERR_OSSL_X509_ISSUER_MISMATCH" | "ERR_OSSL_X509_KEY_TYPE_MISMATCH"
    | "ERR_OSSL_X509_KEY_VALUES_MISMATCH" | "ERR_OSSL_X509_LOADING_CERT_DIR"
    | "ERR_OSSL_X509_LOADING_DEFAULTS" | "ERR_OSSL_X509_METHOD_NOT_SUPPORTED"
    | "ERR_OSSL_X509_NAME_TOO_LONG" | "ERR_OSSL_X509_NEWER_CRL_NOT_NEWER"
    | "ERR_OSSL_X509_NO_CERTIFICATE_FOUND" | "ERR_OSSL_X509_NO_CERTIFICATE_OR_CRL_FOUND"
    | "ERR_OSSL_X509_NO_CERT_SET_FOR_US_TO_VERIFY" | "ERR_OSSL_X509_NO_CRL_FOUND"
    | "ERR_OSSL_X509_NO_CRL_NUMBER" | "ERR_OSSL_X509_PUBLIC_KEY_DECODE_ERROR"
    | "ERR_OSSL_X509_PUBLIC_KEY_ENCODE_ERROR" | "ERR_OSSL_X509_SHOULD_RETRY"
    | "ERR_OSSL_X509_UNABLE_TO_FIND_PARAMETERS_IN_CHAIN"
    | "ERR_OSSL_X509_UNABLE_TO_GET_CERTS_PUBLIC_KEY" | "ERR_OSSL_X509_UNKNOWN_KEY_TYPE"
    | "ERR_OSSL_X509_UNKNOWN_NID" | "ERR_OSSL_X509_UNKNOWN_PURPOSE_ID"
    | "ERR_OSSL_X509_UNKNOWN_SIGID_ALGS" | "ERR_OSSL_X509_UNKNOWN_TRUST_ID"
    | "ERR_OSSL_X509_UNSUPPORTED_ALGORITHM" | "ERR_OSSL_X509_UNSUPPORTED_VERSION"
    | "ERR_OSSL_X509_WRONG_LOOKUP_TYPE" | "ERR_OSSL_X509_WRONG_TYPE"
  export type CreateHashError =
    | { kind: "CREATE_HASH_ERROR_UNKNOWN_ALGORITHM"; code: "ERR_CRYPTO_INVALID_DIGEST"; message: string; algorithm: string }
    | { kind: "CREATE_HASH_ERROR_UNSUPPORTED_ALGORITHM"; code: "ERR_OSSL_EVP_UNSUPPORTED"; message: string; algorithm: string }
    | { kind: "CREATE_HASH_ERROR_WRONG_PARAMETERS"; code: "ERR_OSSL_EVP_NOT_XOF_OR_INVALID_LENGTH" | "ERR_OUT_OF_RANGE"; message: string; parameter: string; value: double }
    | { kind: "CREATE_HASH_ERROR_SYSTEM"; code: "ERR_CRYPTO_OPERATION_FAILED" | "UNKNOWN"; message: string; openssl: string }
  export type HashError =
    | { kind: "HASH_ERROR_UNKNOWN_ALGORITHM"; code: "ERR_CRYPTO_INVALID_DIGEST"; message: string; algorithm: string }
    | { kind: "HASH_ERROR_UNSUPPORTED_ALGORITHM"; code: "ERR_OSSL_EVP_UNSUPPORTED"; message: string; algorithm: string }
    | { kind: "HASH_ERROR_WRONG_PARAMETERS"; code: "ERR_OSSL_EVP_NOT_XOF_OR_INVALID_LENGTH" | "ERR_OUT_OF_RANGE"; message: string; parameter: string; value: double }
    | { kind: "HASH_ERROR_SYSTEM"; code: "ERR_CRYPTO_OPERATION_FAILED" | "UNKNOWN"; message: string; openssl: string }
  export type HashCopyError =
    | { kind: "HASH_COPY_ERROR_WRONG_PARAMETERS"; code: "ERR_OSSL_EVP_NOT_XOF_OR_INVALID_LENGTH" | "ERR_OUT_OF_RANGE"; message: string; parameter: string; value: double }
    | { kind: "HASH_COPY_ERROR_SYSTEM"; code: "ERR_CRYPTO_OPERATION_FAILED" | "UNKNOWN"; message: string; openssl: string }
  export type CreateHmacError =
    | { kind: "CREATE_HMAC_ERROR_UNKNOWN_ALGORITHM"; code: "ERR_CRYPTO_INVALID_DIGEST"; message: string; algorithm: string }
    | { kind: "CREATE_HMAC_ERROR_UNSUPPORTED_ALGORITHM"; code: "ERR_OSSL_EVP_UNSUPPORTED"; message: string; algorithm: string }
    | { kind: "CREATE_HMAC_ERROR_WRONG_KEY_TYPE"; code: "ERR_CRYPTO_INVALID_KEY_OBJECT_TYPE"; message: string; got: "public" | "private"; expected: "secret" }
    | { kind: "CREATE_HMAC_ERROR_SYSTEM"; code: "ERR_CRYPTO_OPERATION_FAILED" | "UNKNOWN"; message: string; openssl: string }
  export type Pbkdf2Error =
    | { kind: "PBKDF2_ERROR_UNKNOWN_ALGORITHM"; code: "ERR_CRYPTO_INVALID_DIGEST"; message: string; algorithm: string }
    | { kind: "PBKDF2_ERROR_UNSUPPORTED_ALGORITHM"; code: "ERR_OSSL_EVP_UNSUPPORTED"; message: string; algorithm: string }
    | { kind: "PBKDF2_ERROR_WRONG_PARAMETERS"; code: "ERR_OUT_OF_RANGE"; message: string; parameter: string; value: double }
    | { kind: "PBKDF2_ERROR_WRONG_KEY_TYPE"; code: "ERR_CRYPTO_INVALID_KEY_OBJECT_TYPE"; message: string; got: "public" | "private"; expected: "secret" }
    | { kind: "PBKDF2_ERROR_SYSTEM"; code: "ERR_CRYPTO_OPERATION_FAILED" | "UNKNOWN"; message: string; openssl: string }
  export type HkdfError =
    | { kind: "HKDF_ERROR_UNKNOWN_ALGORITHM"; code: "ERR_CRYPTO_INVALID_DIGEST"; message: string; algorithm: string }
    | { kind: "HKDF_ERROR_UNSUPPORTED_ALGORITHM"; code: "ERR_OSSL_EVP_UNSUPPORTED"; message: string; algorithm: string }
    | { kind: "HKDF_ERROR_WRONG_PARAMETERS"; code: "ERR_OUT_OF_RANGE" | "ERR_CRYPTO_INVALID_KEYLEN"; message: string; parameter: string; value: double }
    | { kind: "HKDF_ERROR_WRONG_KEY_TYPE"; code: "ERR_CRYPTO_INVALID_KEY_OBJECT_TYPE"; message: string; got: "public" | "private"; expected: "secret" }
    | { kind: "HKDF_ERROR_SYSTEM"; code: "ERR_CRYPTO_OPERATION_FAILED" | "UNKNOWN"; message: string; openssl: string }
  export type ScryptError =
    | { kind: "SCRYPT_ERROR_WRONG_PARAMETERS"; code: "ERR_CRYPTO_INVALID_SCRYPT_PARAMS" | "ERR_OUT_OF_RANGE" | "ERR_OSSL_EVP_MEMORY_LIMIT_EXCEEDED"; message: string; parameter: string; value: double }
    | { kind: "SCRYPT_ERROR_WRONG_KEY_TYPE"; code: "ERR_CRYPTO_INVALID_KEY_OBJECT_TYPE"; message: string; got: "public" | "private"; expected: "secret" }
    | { kind: "SCRYPT_ERROR_SYSTEM"; code: "ERR_CRYPTO_OPERATION_FAILED" | "UNKNOWN"; message: string; openssl: string }
  export type Argon2Error =
    | { kind: "ARGON2_ERROR_UNKNOWN_ALGORITHM"; code: "ERR_INVALID_ARG_VALUE"; message: string; algorithm: string }
    | { kind: "ARGON2_ERROR_WRONG_PARAMETERS"; code: "ERR_OUT_OF_RANGE"; message: string; parameter: string; value: double }
    | { kind: "ARGON2_ERROR_SYSTEM"; code: "ERR_CRYPTO_OPERATION_FAILED" | "UNKNOWN"; message: string; openssl: string }
  export type CreateCipherError =
    | { kind: "CREATE_CIPHER_ERROR_UNKNOWN_ALGORITHM"; code: "ERR_CRYPTO_UNKNOWN_CIPHER"; message: string; algorithm: string }
    | { kind: "CREATE_CIPHER_ERROR_UNSUPPORTED_ALGORITHM"; code: "ERR_OSSL_EVP_UNSUPPORTED"; message: string; algorithm: string }
    | { kind: "CREATE_CIPHER_ERROR_WRONG_KEY_LENGTH"; code: "ERR_CRYPTO_INVALID_KEYLEN"; message: string; algorithm: string; length: int; expected: int }
    | { kind: "CREATE_CIPHER_ERROR_WRONG_IV_LENGTH"; code: "ERR_CRYPTO_INVALID_IV"; message: string; algorithm: string; length: int }
    | { kind: "CREATE_CIPHER_ERROR_WRONG_TAG_LENGTH"; code: "ERR_CRYPTO_INVALID_AUTH_TAG"; message: string; algorithm: string; length: int }
    | { kind: "CREATE_CIPHER_ERROR_WRONG_KEY_TYPE"; code: "ERR_CRYPTO_INVALID_KEY_OBJECT_TYPE"; message: string; got: "public" | "private"; expected: "secret" }
    | { kind: "CREATE_CIPHER_ERROR_SYSTEM"; code: "ERR_CRYPTO_OPERATION_FAILED" | "UNKNOWN"; message: string; openssl: string }
  export type CreateDecipherError =
    | { kind: "CREATE_DECIPHER_ERROR_UNKNOWN_ALGORITHM"; code: "ERR_CRYPTO_UNKNOWN_CIPHER"; message: string; algorithm: string }
    | { kind: "CREATE_DECIPHER_ERROR_UNSUPPORTED_ALGORITHM"; code: "ERR_OSSL_EVP_UNSUPPORTED"; message: string; algorithm: string }
    | { kind: "CREATE_DECIPHER_ERROR_WRONG_KEY_LENGTH"; code: "ERR_CRYPTO_INVALID_KEYLEN"; message: string; algorithm: string; length: int; expected: int }
    | { kind: "CREATE_DECIPHER_ERROR_WRONG_IV_LENGTH"; code: "ERR_CRYPTO_INVALID_IV"; message: string; algorithm: string; length: int }
    | { kind: "CREATE_DECIPHER_ERROR_WRONG_TAG_LENGTH"; code: "ERR_CRYPTO_INVALID_AUTH_TAG"; message: string; algorithm: string; length: int }
    | { kind: "CREATE_DECIPHER_ERROR_WRONG_KEY_TYPE"; code: "ERR_CRYPTO_INVALID_KEY_OBJECT_TYPE"; message: string; got: "public" | "private"; expected: "secret" }
    | { kind: "CREATE_DECIPHER_ERROR_SYSTEM"; code: "ERR_CRYPTO_OPERATION_FAILED" | "UNKNOWN"; message: string; openssl: string }
  export type CipherFinalError =
    | { kind: "CIPHER_FINAL_ERROR_WRONG_DATA_LENGTH"; code: "ERR_OSSL_WRONG_FINAL_BLOCK_LENGTH"; message: string; algorithm: string; length: int }
    | { kind: "CIPHER_FINAL_ERROR_WRONG_ORDER"; code: "ERR_CRYPTO_INVALID_STATE"; message: string; algorithm: string }
    | { kind: "CIPHER_FINAL_ERROR_SYSTEM"; code: "ERR_CRYPTO_OPERATION_FAILED" | "UNKNOWN"; message: string; openssl: string }
  export type DecipherFinalError =
    | { kind: "DECIPHER_FINAL_ERROR_AUTHENTICATION_FAILED"; code: "ERR_CRYPTO_AUTH_FAILED"; message: string; algorithm: string }
    | { kind: "DECIPHER_FINAL_ERROR_BAD_DECRYPT"; code: "ERR_OSSL_BAD_DECRYPT"; message: string; algorithm: string }
    | { kind: "DECIPHER_FINAL_ERROR_WRONG_DATA_LENGTH"; code: "ERR_OSSL_WRONG_FINAL_BLOCK_LENGTH"; message: string; algorithm: string; length: int }
    | { kind: "DECIPHER_FINAL_ERROR_WRONG_TAG_LENGTH"; code: "ERR_CRYPTO_INVALID_AUTH_TAG"; message: string; algorithm: string; length: int }
    | { kind: "DECIPHER_FINAL_ERROR_WRONG_ORDER"; code: "ERR_CRYPTO_INVALID_STATE"; message: string; algorithm: string }
    | { kind: "DECIPHER_FINAL_ERROR_SYSTEM"; code: "ERR_CRYPTO_OPERATION_FAILED" | "UNKNOWN"; message: string; openssl: string }
  export type GetAuthTagError =
    | { kind: "GET_AUTH_TAG_ERROR_WRONG_ORDER"; code: "ERR_CRYPTO_INVALID_STATE"; message: string; algorithm: string }
  export type CreatePrivateKeyError =
    | { kind: "CREATE_PRIVATE_KEY_ERROR_NOT_A_KEY"; code: KeyReadCode; message: string; format: "pem" | "der" | "jwk"; openssl: string }
    | { kind: "CREATE_PRIVATE_KEY_ERROR_WRONG_KEY_TYPE"; code: "ERR_CRYPTO_INVALID_KEY_OBJECT_TYPE"; message: string; got: "public"; expected: "private" }
    | { kind: "CREATE_PRIVATE_KEY_ERROR_PASSPHRASE_MISSING"; code: "ERR_MISSING_PASSPHRASE"; message: string }
    | { kind: "CREATE_PRIVATE_KEY_ERROR_WRONG_PASSPHRASE"; code: "ERR_OSSL_BAD_DECRYPT"; message: string }
    | { kind: "CREATE_PRIVATE_KEY_ERROR_INVALID_JWK"; code: "ERR_CRYPTO_INVALID_JWK"; message: string; field: string }
    | { kind: "CREATE_PRIVATE_KEY_ERROR_SYSTEM"; code: "ERR_CRYPTO_OPERATION_FAILED" | "UNKNOWN"; message: string; openssl: string }
  export type CreatePublicKeyError =
    | { kind: "CREATE_PUBLIC_KEY_ERROR_NOT_A_KEY"; code: KeyReadCode; message: string; format: "pem" | "der" | "jwk"; openssl: string }
    | { kind: "CREATE_PUBLIC_KEY_ERROR_WRONG_KEY_TYPE"; code: "ERR_CRYPTO_INVALID_KEY_OBJECT_TYPE"; message: string; got: "secret"; expected: "public" | "private" }
    | { kind: "CREATE_PUBLIC_KEY_ERROR_PASSPHRASE_MISSING"; code: "ERR_MISSING_PASSPHRASE"; message: string }
    | { kind: "CREATE_PUBLIC_KEY_ERROR_WRONG_PASSPHRASE"; code: "ERR_OSSL_BAD_DECRYPT"; message: string }
    | { kind: "CREATE_PUBLIC_KEY_ERROR_INVALID_JWK"; code: "ERR_CRYPTO_INVALID_JWK"; message: string; field: string }
    | { kind: "CREATE_PUBLIC_KEY_ERROR_SYSTEM"; code: "ERR_CRYPTO_OPERATION_FAILED" | "UNKNOWN"; message: string; openssl: string }
  export type GenerateKeyPairError =
    | { kind: "GENERATE_KEY_PAIR_ERROR_UNKNOWN_ALGORITHM"; code: "ERR_CRYPTO_INVALID_CURVE" | "ERR_INVALID_ARG_VALUE" | "ERR_CRYPTO_UNKNOWN_CIPHER"; message: string; algorithm: string }
    | { kind: "GENERATE_KEY_PAIR_ERROR_WRONG_PARAMETERS"; code: "ERR_OUT_OF_RANGE" | "ERR_OSSL_KEY_SIZE_TOO_SMALL" | "ERR_OSSL_RSA_PUB_EXPONENT_OUT_OF_RANGE" | "ERR_OSSL_BN_BAD_RECURSIVE_CALL"; message: string; parameter: string; value: double }
    | { kind: "GENERATE_KEY_PAIR_ERROR_INCOMPATIBLE_OPTIONS"; code: "ERR_CRYPTO_INCOMPATIBLE_KEY_OPTIONS"; message: string }
    | { kind: "GENERATE_KEY_PAIR_ERROR_PASSPHRASE_MISSING"; code: "ERR_MISSING_PASSPHRASE"; message: string }
    | { kind: "GENERATE_KEY_PAIR_ERROR_SYSTEM"; code: "ERR_CRYPTO_OPERATION_FAILED" | "UNKNOWN"; message: string; openssl: string }
  export type KeyExportError =
    | { kind: "KEY_EXPORT_ERROR_INCOMPATIBLE_OPTIONS"; code: "ERR_CRYPTO_INCOMPATIBLE_KEY_OPTIONS"; message: string }
    | { kind: "KEY_EXPORT_ERROR_CANNOT_EXPORT"; code: "ERR_CRYPTO_JWK_UNSUPPORTED_CURVE" | "ERR_CRYPTO_JWK_UNSUPPORTED_KEY_TYPE"; message: string }
    | { kind: "KEY_EXPORT_ERROR_PASSPHRASE_MISSING"; code: "ERR_MISSING_PASSPHRASE"; message: string }
    | { kind: "KEY_EXPORT_ERROR_UNKNOWN_ALGORITHM"; code: "ERR_CRYPTO_UNKNOWN_CIPHER"; message: string; algorithm: string }
    | { kind: "KEY_EXPORT_ERROR_SYSTEM"; code: "ERR_CRYPTO_OPERATION_FAILED" | "UNKNOWN"; message: string; openssl: string }
  export type SignError =
    | { kind: "SIGN_ERROR_UNKNOWN_ALGORITHM"; code: "ERR_CRYPTO_INVALID_DIGEST" | "ERR_OSSL_EVP_INVALID_DIGEST"; message: string; algorithm: string }
    | { kind: "SIGN_ERROR_UNSUPPORTED_ALGORITHM"; code: "ERR_OSSL_EVP_UNSUPPORTED"; message: string; algorithm: string }
    | { kind: "SIGN_ERROR_WRONG_KEY_TYPE"; code: "ERR_CRYPTO_INVALID_KEY_OBJECT_TYPE"; message: string; got: "public" | "secret"; expected: "private" }
    | { kind: "SIGN_ERROR_NOT_A_KEY"; code: KeyReadCode; message: string; format: "pem" | "der" | "jwk"; openssl: string }
    | { kind: "SIGN_ERROR_PASSPHRASE_MISSING"; code: "ERR_MISSING_PASSPHRASE"; message: string }
    | { kind: "SIGN_ERROR_WRONG_PASSPHRASE"; code: "ERR_OSSL_BAD_DECRYPT"; message: string }
    | { kind: "SIGN_ERROR_SYSTEM"; code: "ERR_CRYPTO_OPERATION_FAILED" | "UNKNOWN"; message: string; openssl: string }
  export type VerifyError =
    | { kind: "VERIFY_ERROR_UNKNOWN_ALGORITHM"; code: "ERR_CRYPTO_INVALID_DIGEST" | "ERR_OSSL_EVP_INVALID_DIGEST"; message: string; algorithm: string }
    | { kind: "VERIFY_ERROR_UNSUPPORTED_ALGORITHM"; code: "ERR_OSSL_EVP_UNSUPPORTED"; message: string; algorithm: string }
    | { kind: "VERIFY_ERROR_WRONG_KEY_TYPE"; code: "ERR_CRYPTO_INVALID_KEY_OBJECT_TYPE"; message: string; got: "secret"; expected: "public" | "private" }
    | { kind: "VERIFY_ERROR_NOT_A_KEY"; code: KeyReadCode; message: string; format: "pem" | "der" | "jwk"; openssl: string }
    | { kind: "VERIFY_ERROR_PASSPHRASE_MISSING"; code: "ERR_MISSING_PASSPHRASE"; message: string }
    | { kind: "VERIFY_ERROR_WRONG_PASSPHRASE"; code: "ERR_OSSL_BAD_DECRYPT"; message: string }
    | { kind: "VERIFY_ERROR_SYSTEM"; code: "ERR_CRYPTO_OPERATION_FAILED" | "UNKNOWN"; message: string; openssl: string }
  export type CreateSignError =
    | { kind: "CREATE_SIGN_ERROR_UNKNOWN_ALGORITHM"; code: "ERR_CRYPTO_INVALID_DIGEST"; message: string; algorithm: string }
    | { kind: "CREATE_SIGN_ERROR_UNSUPPORTED_ALGORITHM"; code: "ERR_OSSL_EVP_UNSUPPORTED"; message: string; algorithm: string }
  export type CreateVerifyError =
    | { kind: "CREATE_VERIFY_ERROR_UNKNOWN_ALGORITHM"; code: "ERR_CRYPTO_INVALID_DIGEST"; message: string; algorithm: string }
    | { kind: "CREATE_VERIFY_ERROR_UNSUPPORTED_ALGORITHM"; code: "ERR_OSSL_EVP_UNSUPPORTED"; message: string; algorithm: string }
  export type SignerSignError =
    | { kind: "SIGNER_SIGN_ERROR_UNSUPPORTED_ALGORITHM"; code: "ERR_CRYPTO_UNSUPPORTED_OPERATION"; message: string; algorithm: string }
    | { kind: "SIGNER_SIGN_ERROR_WRONG_KEY_TYPE"; code: "ERR_CRYPTO_INVALID_KEY_OBJECT_TYPE"; message: string; got: "public" | "secret"; expected: "private" }
    | { kind: "SIGNER_SIGN_ERROR_NOT_A_KEY"; code: KeyReadCode; message: string; format: "pem" | "der" | "jwk"; openssl: string }
    | { kind: "SIGNER_SIGN_ERROR_PASSPHRASE_MISSING"; code: "ERR_MISSING_PASSPHRASE"; message: string }
    | { kind: "SIGNER_SIGN_ERROR_WRONG_PASSPHRASE"; code: "ERR_OSSL_BAD_DECRYPT"; message: string }
    | { kind: "SIGNER_SIGN_ERROR_SYSTEM"; code: "ERR_CRYPTO_OPERATION_FAILED" | "UNKNOWN"; message: string; openssl: string }
    | { kind: "SIGNER_SIGN_ERROR_WRONG_ORDER"; code: "ERR_CRYPTO_INVALID_STATE"; message: string; algorithm: string }
  export type VerifierVerifyError =
    | { kind: "VERIFIER_VERIFY_ERROR_WRONG_KEY_TYPE"; code: "ERR_CRYPTO_INVALID_KEY_OBJECT_TYPE"; message: string; got: "secret"; expected: "public" | "private" }
    | { kind: "VERIFIER_VERIFY_ERROR_NOT_A_KEY"; code: KeyReadCode; message: string; format: "pem" | "der" | "jwk"; openssl: string }
    | { kind: "VERIFIER_VERIFY_ERROR_PASSPHRASE_MISSING"; code: "ERR_MISSING_PASSPHRASE"; message: string }
    | { kind: "VERIFIER_VERIFY_ERROR_WRONG_PASSPHRASE"; code: "ERR_OSSL_BAD_DECRYPT"; message: string }
    | { kind: "VERIFIER_VERIFY_ERROR_SYSTEM"; code: "ERR_CRYPTO_OPERATION_FAILED" | "UNKNOWN"; message: string; openssl: string }
    | { kind: "VERIFIER_VERIFY_ERROR_WRONG_ORDER"; code: "ERR_CRYPTO_INVALID_STATE"; message: string; algorithm: string }
  export type CreateEcdhError =
    | { kind: "CREATE_ECDH_ERROR_UNKNOWN_ALGORITHM"; code: "ERR_CRYPTO_INVALID_CURVE"; message: string; algorithm: string }
  export type EcdhGetPublicKeyError =
    | { kind: "ECDH_GET_PUBLIC_KEY_ERROR_WRONG_ORDER"; code: "ERR_CRYPTO_INVALID_STATE"; message: string; algorithm: string }
  export type EcdhGetPrivateKeyError =
    | { kind: "ECDH_GET_PRIVATE_KEY_ERROR_WRONG_ORDER"; code: "ERR_CRYPTO_INVALID_STATE"; message: string; algorithm: string }
  export type EcdhSetPrivateKeyError =
    | { kind: "ECDH_SET_PRIVATE_KEY_ERROR_WRONG_KEY_TYPE"; code: "ERR_CRYPTO_INVALID_KEYTYPE"; message: string; algorithm: string }
    | { kind: "ECDH_SET_PRIVATE_KEY_ERROR_SYSTEM"; code: "ERR_CRYPTO_OPERATION_FAILED" | "UNKNOWN"; message: string; openssl: string }
  export type EcdhComputeSecretError =
    | { kind: "ECDH_COMPUTE_SECRET_ERROR_INVALID_PUBLIC_KEY"; code: "ERR_CRYPTO_ECDH_INVALID_PUBLIC_KEY"; message: string; algorithm: string }
    | { kind: "ECDH_COMPUTE_SECRET_ERROR_WRONG_ORDER"; code: "ERR_CRYPTO_INVALID_STATE"; message: string; algorithm: string }
    | { kind: "ECDH_COMPUTE_SECRET_ERROR_SYSTEM"; code: "ERR_CRYPTO_OPERATION_FAILED" | "UNKNOWN"; message: string; openssl: string }
  export type DiffieHellmanError =
    | { kind: "DIFFIE_HELLMAN_ERROR_WRONG_KEY_TYPE"; code: "ERR_CRYPTO_INVALID_KEY_OBJECT_TYPE"; message: string; got: "public" | "private" | "secret"; expected: "public" | "private" }
    | { kind: "DIFFIE_HELLMAN_ERROR_INCOMPATIBLE_KEYS"; code: "ERR_CRYPTO_INCOMPATIBLE_KEY"; message: string }
    | { kind: "DIFFIE_HELLMAN_ERROR_SYSTEM"; code: "ERR_CRYPTO_OPERATION_FAILED" | "UNKNOWN"; message: string; openssl: string }
  export type ParseCertificateError =
    | { kind: "PARSE_CERTIFICATE_ERROR_NOT_A_CERTIFICATE"; code: CertificateReadCode; message: string; openssl: string }
    | { kind: "PARSE_CERTIFICATE_ERROR_SYSTEM"; code: "ERR_CRYPTO_OPERATION_FAILED" | "UNKNOWN"; message: string; openssl: string }
  export type CertificateCheckPrivateKeyError =
    | { kind: "CERTIFICATE_CHECK_PRIVATE_KEY_ERROR_WRONG_KEY_TYPE"; code: "ERR_CRYPTO_INVALID_KEY_OBJECT_TYPE"; message: string; got: "public" | "secret"; expected: "private" }
  export type CertificateVerifyError =
    | { kind: "CERTIFICATE_VERIFY_ERROR_WRONG_KEY_TYPE"; code: "ERR_CRYPTO_INVALID_KEY_OBJECT_TYPE"; message: string; got: "private" | "secret"; expected: "public" }
}

declare module "node:fs/promises" {
  import type { Dirent, Stats } from "fs"
  type Path = string | Buffer | URL
  type Data = string | NodeJS.ArrayBufferView
  type Flag =
    | "r" | "rs" | "sr" | "r+" | "rs+" | "sr+"
    | "w" | "wx" | "xw" | "w+" | "wx+" | "xw+"
    | "a" | "ax" | "xa" | "as" | "sa" | "a+" | "ax+" | "xa+" | "as+" | "sa+"
  import type { ReadFileError, WriteFileError, WriteNewFileError, AppendFileError, AppendNewFileError, TruncateError, AccessError, ChangeModeError, StatusError, StatusIfExistsError, LinkStatusError, LinkStatusIfExistsError, ReadDirectoryError, RealPathError, MakeDirectoryError, MakeTemporaryDirectoryError, RemoveDirectoryError, UnlinkError, RemoveError, ForceRemoveError, RenameError, CopyFileError } from "node:fs"
  export type { ReadFileError, WriteFileError, WriteNewFileError, AppendFileError, AppendNewFileError, TruncateError, AccessError, ChangeModeError, StatusError, StatusIfExistsError, LinkStatusError, LinkStatusIfExistsError, ReadDirectoryError, RealPathError, MakeDirectoryError, MakeTemporaryDirectoryError, RemoveDirectoryError, UnlinkError, RemoveError, ForceRemoveError, RenameError, CopyFileError }
  type Done<T, E> = Promise<Result<T, E>>
  type NewFlag = "wx" | "xw" | "wx+" | "xw+"
  type AppendFlag = "a" | "as" | "sa" | "a+" | "as+" | "sa+"
  type AppendNewFlag = "ax" | "xa" | "ax+" | "xa+"
  export function readFile(path: Path, options?: { encoding?: undefined; flag?: Flag }): Done<Buffer, ReadFileError>
  export function readFile(path: Path, options: BufferEncoding | { encoding: BufferEncoding; flag?: Flag }): Done<string, ReadFileError>
  export function writeFile(file: Path, data: Data, options: { encoding?: BufferEncoding; flag: NewFlag; mode?: number }): Done<void, WriteNewFileError>
  export function writeFile(file: Path, data: Data, options: { encoding?: BufferEncoding; flag: AppendFlag; mode?: number }): Done<void, AppendFileError>
  export function writeFile(file: Path, data: Data, options: { encoding?: BufferEncoding; flag: AppendNewFlag; mode?: number }): Done<void, AppendNewFileError>
  export function writeFile(file: Path, data: Data, options?: BufferEncoding | { encoding?: BufferEncoding; flag?: Flag; mode?: number }): Done<void, WriteFileError>
  export function appendFile(file: Path, data: Data, options: { encoding?: BufferEncoding; flag: AppendNewFlag; mode?: number }): Done<void, AppendNewFileError>
  export function appendFile(file: Path, data: Data, options?: BufferEncoding | { encoding?: BufferEncoding; flag?: Flag; mode?: number }): Done<void, AppendFileError>
  export function access(path: Path, mode?: number): Done<void, AccessError>
  export function chmod(path: Path, mode: number | string): Done<void, ChangeModeError>
  export function truncate(path: Path, len?: number): Done<void, TruncateError>
  export function stat(path: Path): Done<Stats, StatusError>
  export function lstat(path: Path): Done<Stats, LinkStatusError>
  export function readdir(path: Path, options: { withFileTypes: true; recursive?: boolean }): Done<Dirent[], ReadDirectoryError>
  export function readdir(path: Path, options?: "utf8" | { encoding?: "utf8"; withFileTypes?: false; recursive?: boolean }): Done<string[], ReadDirectoryError>
  export function mkdir(path: Path, options: { recursive: true; mode?: number }): Done<string | undefined, MakeDirectoryError>
  export function mkdir(path: Path, options?: number | { recursive?: false; mode?: number }): Done<void, MakeDirectoryError>
  export function mkdtemp(prefix: string, options?: "utf8" | { encoding?: "utf8" }): Done<string, MakeTemporaryDirectoryError>
  export function rmdir(path: Path): Done<void, RemoveDirectoryError>
  export function unlink(path: Path): Done<void, UnlinkError>
  export function rm(path: Path, options: { recursive?: boolean; force: true; maxRetries?: number; retryDelay?: number }): Done<void, ForceRemoveError>
  export function rm(path: Path, options?: { recursive?: boolean; force?: boolean; maxRetries?: number; retryDelay?: number }): Done<void, RemoveError>
  export function rename(oldPath: Path, newPath: Path): Done<void, RenameError>
  export function copyFile(src: Path, dest: Path): Done<void, CopyFileError>
  export function realpath(path: Path, options?: "utf8" | { encoding?: "utf8" }): Done<string, RealPathError>
}

// What tz has for files and node:fs has not (docs/io.md, section 5): a move
// over a file that is there, in one step (renameSync and copyFileSync never
// replace a file).
declare module "tz:fs" {
  type Path = string | Buffer | URL
  export type ReplaceError =
    | { kind: "REPLACE_ERROR_NOT_FOUND"; code: "ENOENT"; message: string; syscall: string; path: string; dest: string }
    | { kind: "REPLACE_ERROR_NOT_EMPTY"; code: "ENOTEMPTY" | "EEXIST"; message: string; syscall: string; path: string; dest: string }
    | { kind: "REPLACE_ERROR_WRONG_TYPE"; code: "EISDIR" | "ENOTDIR"; message: string; syscall: string; path: string; dest: string }
    | { kind: "REPLACE_ERROR_PERMISSION"; code: "EACCES" | "EPERM" | "EROFS"; message: string; syscall: string; path: string; dest: string }
    | { kind: "REPLACE_ERROR_BUSY"; code: "EBUSY"; message: string; syscall: string; path: string; dest: string }
    | { kind: "REPLACE_ERROR_FULL"; code: "ENOSPC" | "EMLINK"; message: string; syscall: string; path: string; dest: string }
    | { kind: "REPLACE_ERROR_PATH"; code: "ERR_INVALID_PATH" | "ENAMETOOLONG" | "ELOOP" | "EINVAL"; message: string; syscall: string; path: string; dest: string }
    | { kind: "REPLACE_ERROR_UNSUPPORTED"; code: "EXDEV"; message: string; syscall: string; path: string; dest: string }
    | { kind: "REPLACE_ERROR_SYSTEM"; code: "EIO" | "ENOMEM" | "ETIMEDOUT" | "UNKNOWN"; message: string; syscall: string; path: string; dest: string }
  export function replaceSync(oldPath: Path, newPath: Path): Result<void, ReplaceError>
  export function replace(oldPath: Path, newPath: Path): Promise<Result<void, ReplaceError>>
}

// Input line by line (docs/io.md, section 4).
declare module "tz:io" {
  // Prints the question as it is and reads a line, without its \n or \r\n;
  // undefined at the end of the input.
  export function readLine(question?: string): string | undefined
}
