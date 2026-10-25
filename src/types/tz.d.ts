// What tz has built in, declared for TypeScript's checker in the editor (tsc in
// VS Code): the names a program uses without an import, and the modules
// "tz:io" and "tz:http" (docs/modules.md, section 6). tz itself does not read
// this file; tz build checks the program by its own rules.
//
// It lies next to tz as types/node_modules/@types/tz/index.d.ts, and
// types/tsconfig.json (src/types/tsconfig.json) brings it in together with
// @types/node and neverthrow. A project's tsconfig.json extends
// .tz/tsconfig.json, which tz writes and which extends that one.
//
// Where tz knows more than a declaration can say, the declaration is looser
// than tz, so that the editor does not underline what tz accepts: int, long
// and decimal are number here, and the value of metadata is any. Where
// TypeScript itself is stricter than tz (Json without a check, overloads with
// a body each), the editor underlines code that builds; tz's own language
// server will not.

// Numbers (docs/small.md, docs/decimal.md). For tsc all are number; tz tells
// them apart and checks what mixes them.
type int = number
type long = number
type double = number
type decimal = number

// Rounding modes of decimal (docs/decimal.md, section 4). tz checks a literal
// mode when it compiles the program; a mode in a variable is any string.
type DecimalRoundingMode =
  | "up" | "down" | "ceil" | "floor"
  | "half-up" | "half-down" | "half-even" | "half-ceil" | "half-floor"
  | (string & {})

// decimal("0.1"), decimal(x): an exact decimal from a string or a number.
declare function decimal(value: string | number): decimal
declare namespace decimal {
  function max(...values: decimal[]): decimal
  function min(...values: decimal[]): decimal
  function sum(...values: decimal[]): decimal
}

// The methods of a decimal. Since decimal is number here, numbers get them in
// the editor too; tz rejects them on a double.
interface Number {
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

// JSON (docs/modules.md, section 5; docs/narrowing.md, section 3).
type Json = null | boolean | number | string | Json[] | { [key: string]: Json }

interface JSON {
  // JSON.parse<User>(text) checks every field and gives a User. Without a
  // type argument the rest parameter wants a value of type never, so this
  // declaration does not fit and tsc takes its own JSON.parse.
  parse<T = never>(text: string, ...noTypeArgument: [T] extends [never] ? [never] : []): T
}

// A Json value as type T, every field checked; with a class, made by its
// constructor.
declare function fromJson<T>(json: Json): T
declare function fromJson<T>(cls: abstract new (...args: never[]) => T, json: Json): T

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

// An error of the system: a file that is not there, no access (docs/io.md,
// section 5). Node has no such global; tz does.
declare class SystemError extends Error {
  code: string
  errno: int
  syscall: string
  path?: string
  dest?: string
}

// What execSync and execFileSync of node:child_process throw when the program
// ran and failed (docs/process.md).
declare class ExecSyncError extends Error {
  errno?: int
  code?: string
  syscall?: string
  path?: string
  spawnargs?: string[]
  status?: int
  signal?: string
  output: (string | Buffer | undefined)[]
  pid: int
  stdout: string | Buffer | undefined
  stderr: string | Buffer | undefined
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

// Result of neverthrow (docs/errors.md, section 4): built in, no import
// needed; `import { ok } from "neverthrow"` works too.
type Result<T, E> = import("neverthrow").Result<T, E>
type Ok<T, E> = import("neverthrow").Ok<T, E>
type Err<T, E> = import("neverthrow").Err<T, E>
type ResultAsync<T, E> = import("neverthrow").ResultAsync<T, E>
declare const Result: typeof import("neverthrow").Result
declare const Ok: typeof import("neverthrow").Ok
declare const Err: typeof import("neverthrow").Err
declare const ResultAsync: typeof import("neverthrow").ResultAsync
declare const ok: typeof import("neverthrow").ok
declare const err: typeof import("neverthrow").err
declare const safeTry: typeof import("neverthrow").safeTry
declare const fromThrowable: typeof import("neverthrow").fromThrowable
declare const fromAsyncThrowable: typeof import("neverthrow").fromAsyncThrowable
declare const okAsync: typeof import("neverthrow").okAsync
declare const errAsync: typeof import("neverthrow").errAsync
declare const fromPromise: typeof import("neverthrow").fromPromise
declare const fromSafePromise: typeof import("neverthrow").fromSafePromise

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
    private constructor()
    readonly port: int
    readonly hostname: string
    readonly url: URL
    // Resolved once the server has stopped and its last response is out.
    readonly finished: Promise<void>
    // Stops taking connections and resolves once the requests being handled
    // are answered.
    stop(): Promise<void>
  }

  // Starts a server on `hostname` (0.0.0.0) and `port` (8000; 0 for any free
  // one, which server.port then tells), answering each request with
  // `handler`; of https with `tls`.
  export function serve(options: ServeOptions, handler: Handler): Server
}
