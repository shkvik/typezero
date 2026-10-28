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
            [[  readonly arrayBuffer: () => Promise<Result<ArrayBuffer, BodyError>>]])
patch_types(${types_undici}/fetch.d.ts [[  readonly bytes: () => Promise<Uint8Array>]]
            [[  readonly bytes: () => Promise<Result<Uint8Array, BodyError>>]])
patch_types(${types_undici}/fetch.d.ts [[  readonly formData: () => Promise<FormData>]]
            [[  readonly formData: () => Promise<Result<FormData, BodyError>>]])
patch_types(${types_undici}/fetch.d.ts [[  readonly json: () => Promise<unknown>]]
            [[  readonly json: () => Promise<Result<Json, BodyError | JsonError>>]])
patch_types(${types_undici}/fetch.d.ts [[  readonly text: () => Promise<string>]]
            [[  readonly text: () => Promise<Result<string, BodyError>>]])
patch_types(${types_undici}/fetch.d.ts [[  readonly status?: number]] [[  readonly status?: HttpStatus]])

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
