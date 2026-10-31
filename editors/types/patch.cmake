# What tz's web API does differently from node's (docs/net.md, docs/errors.md,
# docs/objects.md, section 6), changed in the copies of @types/node and
# undici-types that lie next to tz, for TypeScript's checker in the editor. The
# packages in third_party/types stay as npm gives them; a declaration of
# tz.d.ts cannot change what they declare, only add to it.

# patch_types(<file> <old> <new>): <old> must be in <file>, with lines that end
# with \n whatever git gave the file.
function(patch_types file old new)
  file(READ ${file} text)
  string(REPLACE "\r\n" "\n" text "${text}")
  string(FIND "${text}" "${old}" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "patch.cmake: ${file} has no\n${old}")
  endif()
  string(REPLACE "${old}" "${new}" text "${text}")
  file(WRITE ${file} "${text}")
endfunction()

set(types_node ${TZ_OUTPUT_DIR}/types/node_modules/@types/node)
set(types_undici ${TZ_OUTPUT_DIR}/types/node_modules/undici-types)

# fetch and the reading of a body give a Result; bodyUsed() is a method, and a
# status is an HttpStatus.
patch_types(${types_node}/web-globals/fetch.d.ts [[): Promise<Response>;]]
            [[): Promise<Result<Response, FetchError>>;]])
patch_types(${types_undici}/fetch.d.ts [[  readonly bodyUsed: boolean]] [[  bodyUsed (): boolean]])
patch_types(${types_undici}/fetch.d.ts [[  readonly arrayBuffer: () => Promise<ArrayBuffer>]]
            [[  readonly arrayBuffer: () => Promise<Result<ArrayBuffer, BytesError>>]])
patch_types(${types_undici}/fetch.d.ts [[  readonly bytes: () => Promise<Uint8Array>]]
            [[  readonly bytes: () => Promise<Result<Uint8Array, BytesError>>]])
patch_types(${types_undici}/fetch.d.ts [[  readonly formData: () => Promise<FormData>]]
            [[  readonly formData: () => Promise<Result<FormData, FormDataError>>]])
patch_types(${types_undici}/fetch.d.ts [[  readonly json: () => Promise<unknown>]]
            [[  readonly json: () => Promise<Result<Json, JsonBodyError>>]])
patch_types(${types_undici}/fetch.d.ts [[  readonly text: () => Promise<string>]]
            [[  readonly text: () => Promise<Result<string, TextError>>]])
patch_types(${types_undici}/fetch.d.ts [[  readonly status?: number]] [[  readonly status?: HttpStatus]])

# tz has no AbortSignal: a fetch is limited in time by its timeout, in
# milliseconds.
patch_types(${types_undici}/fetch.d.ts [[  signal?: AbortSignal | null]] [[  timeout?: number
  proxy?: { enabled: true; url: string; httpVersion?: "auto" | "1.1" } | { enabled: false }
  httpVersion?: "auto" | "1.1" | "2"]])

# A response tells the version of HTTP it came by (docs/net.md, section 2).
patch_types(${types_undici}/fetch.d.ts [[  readonly redirected: boolean]]
            [[  readonly redirected: boolean
  readonly protocol: "http/1.1" | "h2"]])
patch_types(${types_undici}/fetch.d.ts [[  readonly signal: AbortSignal]]
            [[  readonly timeout?: number]])

# Fields that do something only in a browser are not there (docs/net.md,
# section 2): a request has no mode, cache, credentials, integrity, referrer
# or keepalive.
foreach(field "cache?: RequestCache" "credentials?: RequestCredentials" "dispatcher?: Dispatcher"
              "integrity?: string" "keepalive?: boolean" "mode?: RequestMode" "referrer?: string"
              "referrerPolicy?: ReferrerPolicy" "window?: null" "readonly cache: RequestCache"
              "readonly credentials: RequestCredentials"
              "readonly destination: RequestDestination" "readonly integrity: string"
              "readonly mode: RequestMode" "readonly referrer: string"
              "readonly referrerPolicy: ReferrerPolicy" "readonly keepalive: boolean")
  patch_types(${types_undici}/fetch.d.ts "\n  ${field}\n" "\n")
endforeach()

# URL.parse gives a Result.
patch_types(${types_node}/url.d.ts [[static parse(input: string, base?: string): URL | null;]]
            [[static parse(input: string, base?: string | URL): Result<URL, UrlError>;]])

# A stream of the web reads with a Result, and desiredSize() is a method.
patch_types(${types_node}/stream/web.d.ts [[read(): Promise<ReadableStreamReadResult<R>>;]]
            [[read(): Promise<Result<ReadableStreamReadResult<R>, StreamError>>;]])
patch_types(${types_node}/stream/web.d.ts
            [[values(options?: { preventCancel?: boolean }): ReadableStreamAsyncIterator<R>;
        [Symbol.asyncIterator](): ReadableStreamAsyncIterator<R>;]]
            [[values(options?: { preventCancel?: boolean }): ReadableStreamAsyncIterator<Result<R, StreamError>>;
        [Symbol.asyncIterator](): ReadableStreamAsyncIterator<Result<R, StreamError>>;]])
patch_types(${types_node}/stream/web.d.ts [[interface ReadableStreamDefaultController<R = any> {
        readonly desiredSize: number | null;]]
            [[interface ReadableStreamDefaultController<R = any> {
        desiredSize(): number;]])

# A value that the object changes itself is read through a method
# (docs/objects.md, section 6): the parts of a URL, the size of its
# parameters, the state of an event and of a stream, the body of a request or
# a response.
patch_types(${types_node}/url.d.ts [[`TypeError` will be thrown.
         */
        href: string;]] [[`TypeError` will be thrown.
         */
        href(): string;]])
foreach(part hash host hostname password pathname port protocol search username)
  patch_types(${types_node}/url.d.ts "        ${part}: string;" "        ${part}(): string;")
endforeach()
patch_types(${types_node}/url.d.ts [[        port(): string;]] [[        port(): string;
        portNumber(): int | undefined;]])
patch_types(${types_node}/url.d.ts [[        readonly origin: string;]] [[        origin(): string;]])
patch_types(${types_node}/url.d.ts [[        readonly size: number;]] [[        size(): number;]])
foreach(field "currentTarget: EventTarget | null" "defaultPrevented: boolean" "eventPhase: 0 | 2"
              "isTrusted: boolean" "srcElement: EventTarget | null" "target: EventTarget | null")
  string(REPLACE ":" "():" method "${field}")
  patch_types(${types_node}/web-globals/events.d.ts "    readonly ${field};" "    ${method};")
endforeach()
patch_types(${types_node}/stream/web.d.ts [[interface ReadableStream<R = any> {
        readonly locked: boolean;]] [[interface ReadableStream<R = any> {
        locked(): boolean;]])
patch_types(${types_undici}/fetch.d.ts [[  readonly body: ReadableStream | null]]
            [[  body (): ReadableStream | null]])

# A constructor of Response or Request takes no stream; a stream goes with
# Response.stream or Request.stream (docs/net.md, section 3).
patch_types(${types_undici}/fetch.d.ts [[  constructor (body?: BodyInit, init?: ResponseInit)]]
            [[  constructor (body?: BodyData, init?: ResponseInit)
  static stream (body: ReadableStream<Uint8Array> | undefined, init?: ResponseInit): Response]])
patch_types(${types_undici}/fetch.d.ts [[  constructor (input: RequestInfo, init?: RequestInit)]]
            [[  constructor (input: RequestInfo, init?: RequestOptions)
  static stream (input: RequestInfo, body: ReadableStream<Uint8Array>,
                 init?: RequestOptions): Request]])
foreach(field "readableAborted: boolean" "readableDidRead: boolean"
              "readableEncoding: BufferEncoding | null" "readableEnded: boolean"
              "readableFlowing: boolean | null" "readableHighWaterMark: number"
              "readableLength: number" "readableObjectMode: boolean" "closed: boolean"
              "errored: Error | null" "writable: boolean" "writableAborted: boolean"
              "writableEnded: boolean" "writableFinished: boolean" "writableHighWaterMark: number"
              "writableLength: number" "writableObjectMode: boolean" "writableCorked: number"
              "writableNeedDrain: boolean")
  string(REPLACE ":" "():" method "${field}")
  patch_types(${types_node}/stream.d.ts "            readonly ${field};" "            ${method};")
endforeach()
patch_types(${types_node}/stream.d.ts "            readable: boolean;" "            readable(): boolean;")
patch_types(${types_node}/stream.d.ts "            destroyed: boolean;" "            destroyed(): boolean;")
patch_types(${types_node}/stream.d.ts "            allowHalfOpen: boolean;" "            allowHalfOpen(): boolean;")
patch_types(${types_node}/fs.d.ts "        bytesRead: number;" "        bytesRead(): number;")
patch_types(${types_node}/fs.d.ts "        bytesWritten: number;" "        bytesWritten(): number;")
patch_types(${types_node}/fs.d.ts "        pending: boolean;" "        pending(): boolean;")
