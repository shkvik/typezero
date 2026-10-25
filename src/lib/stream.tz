// node:stream: Readable, Writable, Duplex, Transform, PassThrough, pipe,
// pipeline, finished, compose and Readable.from, ported from
// lib/internal/streams of Node 24 to tz (docs/streams.md).
//
// The state of each side is node's: a ReadableState and a WritableState with
// the same bits and counters, and the functions over (stream, state) take
// node's steps in node's order, so that the events, the calls of _read() and
// _write() and the ticks they wait for are node's. Node's getters
// (readableLength, writableEnded, destroyed...) are fields here, which tz has
// instead of getters: the lengths and high-water marks are kept in them, and
// those that the bits of the states decide are updated where the bits change
// (see sync()).
//
// The chunk type. The code of Stream sees a chunk as an `unknown`, as
// JavaScript does; the classes the program uses, Readable<T>, Writable<T>,
// Duplex<W, R>, Transform<W, R> and PassThrough<T>, are thin: their methods
// give the chunks their types, and they declare no fields Stream does not
// have, so that a Duplex works where a Writable is expected (the compiler
// lets it, src/sema/sema_stream.cpp). A chunk that the stream emits goes to
// the listeners of 'data' as what it is: a Buffer, a string once the stream
// has an encoding, or a T in object mode; the compiler makes sure the
// listeners take that (setEncoding narrows the type of the stream from that
// point on, and is an error on a stream that has listeners already).
//
// Cycles. tz frees memory by counting references, which a cycle keeps alive
// (README, "Строки и память"). The listeners this code adds to a stream
// (finished(), pipe(), pipeline()) take the stream as `this` rather than
// capture it, or are removed when the stream ends, so that a stream that
// ends leaves no cycle behind; where node leaves such a listener in place,
// that is said where it is done.

import { EventEmitterBase } from "node:events"
import { AbortController, AbortSignal, Event } from "tz:web"
import { StringDecoder, normalizeEncoding } from "node:string_decoder"

//===----------------------------------------------------------------------===//
// Errors (lib/internal/errors.js)
//===----------------------------------------------------------------------===//

// An error of node with its code, e.g. ERR_STREAM_PUSH_AFTER_EOF.
class StreamError extends Error {
  code: string
  constructor(code: string, message: string) {
    super(message)
    this.code = code
  }
}

class StreamTypeError extends TypeError {
  code: string
  constructor(code: string, message: string) {
    super(message)
    this.code = code
  }
}

class StreamRangeError extends RangeError {
  code: string
  constructor(code: string, message: string) {
    super(message)
    this.code = code
  }
}

// What this code asks of the runtime (runtime/library.c, inspect.c): the
// name of the class of an object; an unknown as console.log shows it.
function __tz_lib_class_name(object: object): string
function __tz_lib_inspect(value: unknown): string

// node's AbortError.
export class AbortError extends Error {
  code = "ABORT_ERR"
  constructor(message = "The operation was aborted", cause?: Error) {
    super(message, { cause })
    this.name = "AbortError"
  }
}

function errorOf(reason: unknown): Error | undefined {
  return reason instanceof Error ? reason : undefined
}

// How node's ERR_INVALID_ARG_TYPE describes the value it got.
function received(value: unknown): string {
  if (value == undefined) return "Received undefined"
  if (typeof value == "function") return "Received function"
  if (typeof value == "object") return `Received an instance of ${__tz_lib_class_name(value)}`
  let shown = __tz_lib_inspect(value)
  if (shown.length > 28) shown = `${shown.slice(0, 25)}...`
  return `Received type ${typeof value} (${shown})`
}

function invalidChunk(value: unknown) {
  return new StreamTypeError(
    "ERR_INVALID_ARG_TYPE",
    'The "chunk" argument must be of type string or an instance of Buffer, TypedArray, or ' +
      `DataView. ${received(value)}`,
  )
}

function methodNotImplemented(method: string) {
  return new StreamError("ERR_METHOD_NOT_IMPLEMENTED", `The ${method} method is not implemented`)
}

function streamDestroyed(method: string) {
  return new StreamError(
    "ERR_STREAM_DESTROYED",
    `Cannot call ${method} after a stream was destroyed`,
  )
}

function multipleCallback() {
  return new StreamError("ERR_MULTIPLE_CALLBACK", "Callback called multiple times")
}

function prematureClose() {
  return new StreamError("ERR_STREAM_PREMATURE_CLOSE", "Premature close")
}

function unknownEncoding(encoding: string) {
  return new StreamTypeError("ERR_UNKNOWN_ENCODING", `Unknown encoding: ${encoding}`)
}

function nullValues() {
  return new StreamTypeError("ERR_STREAM_NULL_VALUES", "May not write null values to stream")
}

// node's aggregateTwoErrors.
function aggregateTwoErrors(inner: Error | undefined, outer: Error | undefined) {
  if (inner != undefined && outer != undefined && inner != outer) {
    const errors = inner instanceof AggregateStreamError ? [...inner.errors, outer] : [inner, outer]
    const err = new AggregateStreamError(errors, inner.message)
    return err
  }
  return inner ?? outer
}

class AggregateStreamError extends Error {
  code: string | undefined = undefined
  constructor(
    readonly errors: Error[],
    message: string,
  ) {
    super(message)
    this.name = "AggregateError"
    const first = errors[0]
    if (first instanceof StreamError || first instanceof StreamTypeError) this.code = first.code
  }
}

function codeOf(err: Error | undefined): string {
  if (err instanceof StreamError || err instanceof StreamTypeError ||
      err instanceof StreamRangeError)
    return err.code
  if (err instanceof AbortError) return err.code
  if (err instanceof AggregateStreamError) return err.code ?? ""
  return ""
}

//===----------------------------------------------------------------------===//
// The state of the sides (lib/internal/streams/utils.js, readable.js and
// writable.js): the bits of node
//===----------------------------------------------------------------------===//

const kObjectMode = 1 << 0
const kErrorEmitted = 1 << 1
const kAutoDestroy = 1 << 2
const kEmitClose = 1 << 3
const kDestroyed = 1 << 4
const kClosed = 1 << 5
const kCloseEmitted = 1 << 6
const kErrored = 1 << 7
const kConstructed = 1 << 8

// Readable.
const kEnded = 1 << 9
const kEndEmitted = 1 << 10
const kReading = 1 << 11
const kSync = 1 << 12
const kNeedReadable = 1 << 13
const kEmittedReadable = 1 << 14
const kReadableListening = 1 << 15
const kResumeScheduled = 1 << 16
const kMultiAwaitDrain = 1 << 17
const kReadingMore = 1 << 18
const kDataEmitted = 1 << 19
const kDecoder = 1 << 21
const kHasFlowing = 1 << 23
const kFlowing = 1 << 24
const kHasPaused = 1 << 25
const kPaused = 1 << 26
const kDataListening = 1 << 27

// Writable.
const kWSync = 1 << 9
const kFinalCalled = 1 << 10
const kNeedDrain = 1 << 11
const kEnding = 1 << 12
const kFinished = 1 << 13
const kDecodeStrings = 1 << 14
const kWriting = 1 << 15
const kBufferProcessing = 1 << 16
const kPrefinished = 1 << 17
const kAllBuffers = 1 << 18
const kAllNoop = 1 << 19
const kOnFinished = 1 << 20
const kCorked = 1 << 23
const kExpectWriteCb = 1 << 26
const kAfterWriteTickInfo = 1 << 27
const kAfterWritePending = 1 << 28
const kBuffered = 1 << 29
const kWEnded = 1 << 30

// The kinds of listeners of src/lib/events.ts: one that takes the emitter as
// `this`, and one that takes the first argument of its event as an unknown.
const kThisListener = 2
const kUnknownArgListener = 3

// The callback of a write, of end() and of the hooks.
export type StreamCallback = (error?: Error) => void

// The callback of _transform() and _flush(): an error, or the chunk to push,
// if any.
export type TransformCallback = (error?: Error, data?: unknown) => void

function nop(_error?: Error) {}

class ReadableState {
  bits: int
  buffer: unknown[] = []
  bufferIndex: int = 0
  pipes: Stream[] = []
  // node's awaitDrainWriters: one writer, or a set of them with
  // kMultiAwaitDrain.
  awaitDrainWriter: Stream | undefined = undefined
  awaitDrainWriters = new Set<Stream>()
  decoder: StringDecoder | undefined = undefined
  encoding: string | undefined = undefined
  defaultEncoding = "utf8"
  errored: Error | undefined = undefined
  // false: the readable side of a Duplex that has none.
  readable: boolean | undefined = undefined
  // The public bits the fields of the stream show (see sync()); -1 for none.
  synced: int = -1
  constructor(bits: int) {
    this.bits = bits
  }
}

interface BufferedWrite {
  chunk: unknown
  encoding: string
  callback: StreamCallback
}

// What a write that finished at once leaves for the next tick.
class AfterWriteTick {
  count: int = 1
  constructor(readonly cb: StreamCallback) {}
}

class WritableState {
  bits: int
  writelen: int = 0
  buffered: (BufferedWrite | undefined)[] = []
  bufferedIndex: int = 0
  pendingcb: int = 0
  writecb: StreamCallback = nop
  afterWriteTickInfo: AfterWriteTick | undefined = undefined
  onFinished: StreamCallback[] = []
  errored: Error | undefined = undefined
  defaultEncoding = "utf8"
  // false: the writable side of a Duplex that has none.
  writable: boolean | undefined = undefined
  // The public bits the fields of the stream show (see sync()); -1 for none.
  synced: int = -1
  constructor(bits: int) {
    this.bits = bits
  }
}

//===----------------------------------------------------------------------===//
// lib/internal/streams/state.js
//===----------------------------------------------------------------------===//

// 16 KiB on Windows, as node has it there, 64 KiB elsewhere.
let defaultHighWaterMarkBytes: int = process.platform == "win32" ? 16 * 1024 : 64 * 1024
let defaultHighWaterMarkObjectMode: int = 16

export function getDefaultHighWaterMark(objectMode: boolean): int {
  return objectMode ? defaultHighWaterMarkObjectMode : defaultHighWaterMarkBytes
}

export function setDefaultHighWaterMark(objectMode: boolean, value: int) {
  if (value < 0)
    throw new StreamRangeError(
      "ERR_OUT_OF_RANGE",
      `The value of "value" is out of range. It must be >= 0 && <= 9007199254740991. Received ${value}`,
    )
  if (objectMode) defaultHighWaterMarkObjectMode = value
  else defaultHighWaterMarkBytes = value
}

function getHighWaterMark(hwm: double | undefined, name: string, objectMode: boolean): int {
  if (hwm != undefined) {
    if (!Number.isInteger(hwm) || hwm < 0)
      throw new StreamTypeError(
        "ERR_INVALID_ARG_VALUE",
        `The property '${name}' is invalid. Received ${hwm}`,
      )
    return Math.floor(hwm) | 0
  }
  return getDefaultHighWaterMark(objectMode)
}

// What the classes give Stream to make each side: node's options, those
// the state of a side takes.
interface SideOptions {
  highWaterMark?: double
  sideHighWaterMark?: double
  objectMode?: boolean
  sideObjectMode?: boolean
  encoding?: string
  defaultEncoding?: string
  decodeStrings?: boolean
  emitClose?: boolean
  autoDestroy?: boolean
}

//===----------------------------------------------------------------------===//
// Stream: the code of every kind of stream
//===----------------------------------------------------------------------===//

// node's legacy Stream, and the base of all streams: the states of the sides
// it has and node's methods over them.
export class Stream extends EventEmitterBase {
  @__tz_hidden _readableState: ReadableState | undefined = undefined
  @__tz_hidden _writableState: WritableState | undefined = undefined
  @__tz_hidden allowHalfOpen = true
  // What node's getters give.
  @__tz_hidden destroyed = false
  @__tz_hidden closed = false
  @__tz_hidden errored: Error | undefined = undefined
  @__tz_hidden readable = false
  @__tz_hidden readableAborted = false
  @__tz_hidden readableDidRead = false
  @__tz_hidden readableEncoding: string | undefined = undefined
  @__tz_hidden readableEnded = false
  @__tz_hidden readableFlowing: boolean | undefined = undefined
  @__tz_hidden readableHighWaterMark: int = 0
  @__tz_hidden readableLength: int = 0
  @__tz_hidden readableObjectMode = false
  @__tz_hidden writable = false
  @__tz_hidden writableAborted = false
  @__tz_hidden writableCorked: int = 0
  @__tz_hidden writableEnded = false
  @__tz_hidden writableFinished = false
  @__tz_hidden writableHighWaterMark: int = 0
  @__tz_hidden writableLength: int = 0
  @__tz_hidden writableNeedDrain = false
  @__tz_hidden writableObjectMode = false
  // The waits of the stream that node keeps as listeners of symbols:
  // kConstruct and kDestroy.
  @__tz_hidden _onConstruct: (() => void)[] = []
  @__tz_hidden _onDestroy: ((err: Error | undefined) => void)[] = []
  // Whether the hooks node finds by their presence are there: _construct,
  // _writev, _final and _flush.
  @__tz_hidden _hasConstruct = false
  @__tz_hidden _hasWritev = false
  @__tz_hidden _hasFinal = false
  @__tz_hidden _hasFlush = false
  @__tz_hidden _isTransform = false
  // A Transform's callback that waits for the reader (node's kCallback).
  @__tz_hidden _transformCallback: StreamCallback | undefined = undefined

  constructor(options?: { captureRejections?: boolean }) {
    super(options)
  }

  // ReadableState of node, made by the classes that read.
  _initReadable(options: SideOptions, isDuplex: boolean) {
    let bits = kEmitClose | kAutoDestroy | kConstructed | kSync
    const objectMode =
      (options.objectMode ?? false) || (isDuplex && (options.sideObjectMode ?? false))
    if (objectMode) bits |= kObjectMode
    const state = new ReadableState(bits)
    this.readableHighWaterMark = getHighWaterMark(
      options.highWaterMark ?? (isDuplex ? options.sideHighWaterMark : undefined),
      isDuplex ? "options.readableHighWaterMark" : "options.highWaterMark",
      objectMode,
    )
    if (options.emitClose == false) state.bits &= ~kEmitClose
    if (options.autoDestroy == false) state.bits &= ~kAutoDestroy
    const defaultEncoding = options.defaultEncoding
    if (defaultEncoding != undefined) {
      if (!Buffer.isEncoding(defaultEncoding)) throw unknownEncoding(defaultEncoding)
      state.defaultEncoding = normalizeEncoding(defaultEncoding)
    }
    const encoding = options.encoding
    if (encoding != undefined && encoding != "") {
      state.decoder = new StringDecoder(encoding)
      state.bits |= kDecoder
      state.encoding = state.decoder.encoding
    }
    this._readableState = state
    this.readableObjectMode = objectMode
    sync(this)
  }

  // WritableState of node.
  _initWritable(options: SideOptions, isDuplex: boolean) {
    let bits = kWSync | kConstructed | kEmitClose | kAutoDestroy
    const objectMode =
      (options.objectMode ?? false) || (isDuplex && (options.sideObjectMode ?? false))
    if (objectMode) bits |= kObjectMode
    const state = new WritableState(bits)
    this.writableHighWaterMark = getHighWaterMark(
      options.highWaterMark ?? (isDuplex ? options.sideHighWaterMark : undefined),
      isDuplex ? "options.writableHighWaterMark" : "options.highWaterMark",
      objectMode,
    )
    if (options.decodeStrings != false) state.bits |= kDecodeStrings
    if (options.emitClose == false) state.bits &= ~kEmitClose
    if (options.autoDestroy == false) state.bits &= ~kAutoDestroy
    const defaultEncoding = options.defaultEncoding
    if (defaultEncoding != undefined) {
      if (!Buffer.isEncoding(defaultEncoding)) throw unknownEncoding(defaultEncoding)
      state.defaultEncoding = normalizeEncoding(defaultEncoding)
    }
    resetBuffer(state)
    this._writableState = state
    this.writableObjectMode = objectMode
    sync(this)
  }

  // The end of node's constructors: the signal, and _construct().
  _initEnd(signal: AbortSignal | undefined) {
    if (signal != undefined) addAbortSignalNoValidate(signal, this)
    if (this._hasConstruct) {
      const self = this
      construct(this, () => {
        const r = self._readableState
        if (r != undefined && (r.bits & kNeedReadable) != 0) maybeReadMore(self, r)
        const w = self._writableState
        if (w != undefined) {
          if ((w.bits & kWriting) == 0) clearBuffer(self, w)
          if ((w.bits & kEnding) != 0) finishMaybe(self, w, false)
        }
      })
    }
  }

  //===--------------------------------------------------------------------===//
  // The hooks, which the classes and the program's classes replace
  //===--------------------------------------------------------------------===//

  _read(size: int): void {
    void size
    throw methodNotImplemented("_read()")
  }

  // _write() and _writev() of the chunk as it is, which the classes pass on
  // to those of their chunk type.
  _writeChunk(chunk: unknown, encoding: string, callback: StreamCallback): void {
    void chunk
    void encoding
    void callback
    throw methodNotImplemented("_write()")
  }

  _writevChunks(chunks: BufferedWrite[], callback: StreamCallback): void {
    void chunks
    void callback
    throw methodNotImplemented("_writev()")
  }

  _final(callback: StreamCallback): void {
    callback()
  }

  _destroy(error: Error | undefined, callback: StreamCallback): void {
    callback(error)
  }

  _construct(callback: StreamCallback): void {
    callback()
  }

  // _flush() of a Transform.
  _flush(callback: TransformCallback): void {
    callback()
  }

  // Whether _final is Transform's own (node's `this._final === final`),
  // which Writable calls; or the stream is no Transform.
  _ownTransformFinal(): boolean {
    return true
  }

  // A 'data' event of a chunk: the class gives it the type its listeners
  // take (see the top of this file).
  _emitData(chunk: unknown): void {
    this.emit("data", chunk)
  }

  //===--------------------------------------------------------------------===//
  // Events: node's Readable.prototype.on and the others
  //===--------------------------------------------------------------------===//

  on(type: string, listener: unknown, kind: int = 0) {
    const res = super.on(type, listener, kind)
    const state = this._readableState
    if (state == undefined) return res
    if (type == "data") {
      state.bits |= kDataListening
      // Update readableListening so that resume() may be a no-op a few
      // lines down. This is needed to support once('readable').
      state.bits |= this.listenerCount("readable") > 0 ? kReadableListening : 0
      // Try start flowing on next tick if stream isn't explicitly paused.
      if ((state.bits & (kHasFlowing | kFlowing)) != kHasFlowing) this.resume()
    } else if (type == "readable") {
      if ((state.bits & (kEndEmitted | kReadableListening)) == 0) {
        state.bits |= kReadableListening | kNeedReadable | kHasFlowing
        state.bits &= ~(kFlowing | kEmittedReadable)
        sync(this)
        if (this.readableLength != 0) emitReadable(this)
        else if ((state.bits & kReading) == 0) process.nextTick(() => this._readChunk(0))
      }
    }
    return res
  }

  addListener(type: string, listener: unknown, kind: int = 0) {
    return this.on(type, listener, kind)
  }

  removeListener(type: string, listener: unknown) {
    const res = super.removeListener(type, listener)
    const state = this._readableState
    if (state == undefined) return res
    if (type == "readable") {
      // We need to check if there is someone still listening to readable and
      // reset the state. However this needs to happen after readable has been
      // emitted but before I/O (nextTick) to support once('readable', fn)
      // cycles. This means that calling resume within the same tick will have
      // no effect.
      process.nextTick(() => updateReadableListening(this))
    } else if (type == "data" && this.listenerCount("data") == 0) {
      state.bits &= ~kDataListening
    }
    return res
  }

  removeAllListeners(type?: string) {
    const res = super.removeAllListeners(type)
    if (this._readableState != undefined && (type == "readable" || type == undefined))
      process.nextTick(() => updateReadableListening(this))
    return res
  }

  //===--------------------------------------------------------------------===//
  // Reading
  //===--------------------------------------------------------------------===//

  // push() and unshift() of a chunk as it is; undefined ends the stream,
  // as null does in node.
  _pushChunk(chunk: unknown, encoding?: string): boolean {
    const state = readableStateOf(this)
    const ret =
      (state.bits & kObjectMode) == 0
        ? readableAddChunkPushByteMode(this, state, chunk, encoding)
        : readableAddChunkPushObjectMode(this, state, chunk, encoding)
    sync(this)
    return ret
  }

  _unshiftChunk(chunk: unknown, encoding?: string): boolean {
    const state = readableStateOf(this)
    const ret =
      (state.bits & kObjectMode) == 0
        ? readableAddChunkUnshiftByteMode(this, state, chunk, encoding)
        : readableAddChunkUnshiftObjectMode(this, state, chunk)
    sync(this)
    return ret
  }

  isPaused(): boolean {
    const state = this._readableState
    if (state == undefined) return false
    return (state.bits & kPaused) != 0 || (state.bits & (kHasFlowing | kFlowing)) == kHasFlowing
  }

  setEncoding(encoding?: string) {
    const state = readableStateOf(this)
    const decoder = new StringDecoder(encoding)
    state.decoder = decoder
    state.bits |= kDecoder
    // If setEncoding(null), decoder.encoding equals utf8.
    state.encoding = decoder.encoding
    // Iterate over current buffer to convert already stored Buffers:
    let content = ""
    for (let i = state.bufferIndex; i < state.buffer.length; i++) {
      const data = state.buffer[i]
      content += typeof data == "string" ? data : decoder.write(bufferOf(data))
    }
    state.buffer = []
    state.bufferIndex = 0
    if (content != "") state.buffer.push(content)
    this.readableLength = content.length
    state.synced = -1 // the encoding shows
    sync(this)
    return this
  }

  // read(n) of node, which gives undefined where node gives null.
  _readChunk(size?: int): unknown {
    const ret = read(this, readableStateOf(this), size)
    sync(this)
    return ret
  }

  pipe<D extends Stream>(dest: D, options?: { end?: boolean }): D {
    if (this._readableState == undefined) {
      // A Writable has no pipe(), which node says thus.
      errorOrDestroy(this, new StreamError("ERR_STREAM_CANNOT_PIPE", "Cannot pipe, not readable"))
      return dest
    }
    pipe(this, dest, options?.end ?? true)
    return dest
  }

  unpipe(dest?: Stream) {
    const state = readableStateOf(this)
    // If we're not piping anywhere, then do nothing.
    if (state.pipes.length == 0) return this
    if (dest == undefined) {
      // remove all.
      const dests = state.pipes
      state.pipes = []
      this.pause()
      for (const d of dests) d.emit("unpipe", this, { hasUnpiped: false })
      return this
    }
    // Try to find the right one.
    const index = state.pipes.indexOf(dest)
    if (index == -1) return this
    state.pipes.splice(index, 1)
    if (state.pipes.length == 0) this.pause()
    dest.emit("unpipe", this, { hasUnpiped: false })
    return this
  }

  // pause() and resume() are remnants of the legacy readable stream API. If
  // the user uses them, then switch into old mode.
  resume() {
    const state = readableStateOf(this)
    if ((state.bits & kFlowing) == 0) {
      // We flow only if there is no one listening for readable, but we still
      // have to call resume().
      state.bits |= kHasFlowing
      if ((state.bits & kReadableListening) == 0) state.bits |= kFlowing
      else state.bits &= ~kFlowing
      resume(this, state)
    }
    state.bits |= kHasPaused
    state.bits &= ~kPaused
    sync(this)
    return this
  }

  pause() {
    const state = readableStateOf(this)
    if ((state.bits & (kHasFlowing | kFlowing)) != kHasFlowing) {
      state.bits |= kHasFlowing
      state.bits &= ~kFlowing
      sync(this)
      this.emit("pause")
    }
    state.bits |= kHasPaused | kPaused
    return this
  }

  //===--------------------------------------------------------------------===//
  // Writing
  //===--------------------------------------------------------------------===//

  // write() of a chunk as it is: node's _write(), which gives an Error
  // where the write failed at once.
  _writeAny(chunk: unknown, encoding: string | undefined, cb: StreamCallback): boolean | Error {
    const ret = writeImpl(this, chunk, encoding, cb)
    sync(this)
    return ret
  }

  // node's state.onwrite, the callback of each _write(): a method of the
  // stream, which needs no closure of its own.
  _onwrite(er?: Error): void {
    onwrite(this, er)
  }

  // write(chunk, encoding, cb) of the program (src/sema/sema_stream.cpp).
  _writeBool(chunk: unknown, encoding: string | undefined, cb: StreamCallback | undefined): boolean {
    return this._writeAny(chunk, encoding, cb ?? nop) == true
  }

  cork() {
    const state = writableStateOf(this)
    state.bits |= kCorked
    this.writableCorked++
  }

  uncork() {
    const state = writableStateOf(this)
    if (this.writableCorked != 0) {
      this.writableCorked--
      if (this.writableCorked == 0) state.bits &= ~kCorked
      if ((state.bits & kWriting) == 0) clearBuffer(this, state)
    }
    sync(this)
  }

  setDefaultEncoding(encoding: string) {
    const lower = encoding.toLowerCase()
    if (!Buffer.isEncoding(lower)) throw unknownEncoding(lower)
    writableStateOf(this).defaultEncoding = normalizeEncoding(lower)
    return this
  }

  // end(chunk, encoding, cb) of node.
  _endWith(hasChunk: boolean, chunk: unknown, encoding: string | undefined,
           cb: StreamCallback | undefined) {
    const state = writableStateOf(this)
    let err: Error | undefined = undefined
    if (hasChunk) {
      const ret = writeImpl(this, chunk, encoding, nop)
      if (ret instanceof Error) err = ret
    }
    // .end() fully uncorks.
    if ((state.bits & kCorked) != 0) {
      this.writableCorked = 1
      this.uncork()
    }
    if (err != undefined) {
      // Do nothing...
    } else if ((state.bits & (kEnding | kErrored)) == 0) {
      // This is forgiving in terms of unnecessary calls to end() and can hide
      // logic errors. However, usually such errors are harmless and causing a
      // hard error can be disproportionately destructive. It is not always
      // trivial for the user to determine whether end() needs to be called
      // or not.
      state.bits |= kEnding
      sync(this)
      finishMaybe(this, state, true)
      state.bits |= kWEnded
    } else if ((state.bits & kFinished) != 0) {
      err = new StreamError(
        "ERR_STREAM_ALREADY_FINISHED",
        "Cannot call end after a stream was finished",
      )
    } else if ((state.bits & kDestroyed) != 0) {
      err = streamDestroyed("end")
    }
    if (cb != undefined) {
      const callback = cb
      const failed = err
      if (failed != undefined) {
        process.nextTick(() => callback(failed))
      } else if ((state.bits & kErrored) != 0) {
        const errored = state.errored
        process.nextTick(() => callback(errored))
      } else if ((state.bits & kFinished) != 0) {
        process.nextTick(() => callback())
      } else {
        state.bits |= kOnFinished
        state.onFinished.push(callback)
      }
    }
    sync(this)
    return this
  }

  //===--------------------------------------------------------------------===//
  // Destroying (lib/internal/streams/destroy.js)
  //===--------------------------------------------------------------------===//

  destroy(error?: Error, callback?: StreamCallback) {
    const w = this._writableState
    // Invoke pending callbacks (Writable.prototype.destroy).
    if (w != undefined && (w.bits & (kBuffered | kOnFinished)) != 0 &&
        (w.bits & kDestroyed) == 0) {
      const state = w
      process.nextTick(() => errorBuffer(this, state))
    }
    destroyImpl(this, error, callback)
    return this
  }

  // process.stdout and process.stderr, which pipe() does not end.
  _markStdio() {
    stdioStreams.push(this)
  }

  _undestroy() {
    const r = this._readableState
    const w = this._writableState
    if (r != undefined) {
      r.bits |= kConstructed
      r.bits &= ~(kClosed | kCloseEmitted | kDestroyed | kErrored | kErrorEmitted | kReading)
      r.errored = undefined
      if (r.readable == false) r.bits |= kEnded | kEndEmitted
      else r.bits &= ~(kEnded | kEndEmitted)
    }
    if (w != undefined) {
      w.bits |= kConstructed
      w.bits &= ~(kDestroyed | kClosed | kCloseEmitted | kErrored | kErrorEmitted | kFinalCalled |
        kPrefinished)
      w.errored = undefined
      if (w.writable == false) w.bits |= kWEnded | kEnding | kFinished
      else w.bits &= ~(kWEnded | kEnding | kFinished)
    }
    sync(this)
  }
}

// The state of the readable side, which the caller knows is there.
function readableStateOf(stream: Stream): ReadableState {
  const state = stream._readableState
  if (state == undefined) throw new StreamTypeError("ERR_INVALID_THIS", "the stream is not readable")
  return state
}

function writableStateOf(stream: Stream): WritableState {
  const state = stream._writableState
  if (state == undefined) throw new StreamTypeError("ERR_INVALID_THIS", "the stream is not writable")
  return state
}

// Node's getters, from the states: the fields of the stream that hold them.
// The bits of each side that node's getters read: sync() does nothing while
// they stay as they were, which they do on most calls.
const kReadablePublic = kEndEmitted | kHasFlowing | kFlowing | kDataEmitted | kDestroyed |
  kErrorEmitted | kErrored | kClosed | kDecoder
const kWritablePublic = kEnding | kFinished | kDestroyed | kNeedDrain | kWEnded | kErrored | kClosed

function sync(stream: Stream) {
  const r = stream._readableState
  const w = stream._writableState
  if ((r != undefined && r.synced != (r.bits & kReadablePublic)) ||
      (w != undefined && w.synced != (w.bits & kWritablePublic)))
    syncNow(stream, r, w)
}

function syncNow(stream: Stream, r: ReadableState | undefined, w: WritableState | undefined) {
  if (r != undefined) r.synced = r.bits & kReadablePublic
  if (w != undefined) w.synced = w.bits & kWritablePublic
  if (r != undefined) {
    const b = r.bits
    stream.readableEnded = (b & kEndEmitted) != 0
    stream.readableFlowing = (b & kHasFlowing) != 0 ? (b & kFlowing) != 0 : undefined
    stream.readableDidRead = (b & kDataEmitted) != 0
    stream.readableEncoding = r.encoding
    stream.readable = r.readable != false && (b & (kDestroyed | kErrorEmitted | kEndEmitted)) == 0
    stream.readableAborted =
      r.readable != false && ((b & kDestroyed) != 0 || r.errored != undefined) &&
      (b & kEndEmitted) == 0
  }
  if (w != undefined) {
    const b = w.bits
    stream.writableEnded = (b & kEnding) != 0
    stream.writableFinished = (b & kFinished) != 0
    stream.writableNeedDrain = (b & (kDestroyed | kEnding | kNeedDrain)) == kNeedDrain
    stream.writable = w.writable != false && (b & (kEnding | kWEnded | kDestroyed | kErrored)) == 0
    stream.writableAborted =
      w.writable != false && (b & (kDestroyed | kErrored)) != 0 && (b & kFinished) == 0
  }
  // A Duplex is destroyed when both sides are; its errored is that of the
  // readable side.
  if (r != undefined && w != undefined) {
    stream.destroyed = (r.bits & kDestroyed) != 0 && (w.bits & kDestroyed) != 0
    stream.closed = (r.bits & kClosed) != 0
    stream.errored = r.errored
  } else if (r != undefined) {
    stream.destroyed = (r.bits & kDestroyed) != 0
    stream.closed = (r.bits & kClosed) != 0
    stream.errored = r.errored
  } else if (w != undefined) {
    stream.destroyed = (w.bits & kDestroyed) != 0
    stream.closed = (w.bits & kClosed) != 0
    stream.errored = w.errored
  }
}

// A chunk of a stream of bytes, which is a Buffer.
function bufferOf(chunk: unknown): Buffer {
  return __tz_chunk<Buffer>(chunk)
}

function lengthOf(chunk: unknown): int {
  if (typeof chunk == "string") return chunk.length
  return bufferOf(chunk).length
}

//===----------------------------------------------------------------------===//
// lib/internal/streams/readable.js
//===----------------------------------------------------------------------===//

function readableAddChunkUnshiftByteMode(stream: Stream, state: ReadableState, chunk: unknown,
                                         encoding: string | undefined): boolean {
  if (chunk == undefined) {
    state.bits &= ~kReading
    onEofChunk(stream, state)
    return false
  }
  let value = chunk
  if (typeof chunk == "string") {
    const enc = encoding == undefined || encoding == "" ? state.defaultEncoding : encoding
    if (state.encoding != normalizeEncoding(enc)) {
      const encoded = Buffer.from(chunk, enc)
      // When unshifting, if state.encoding is set, we have to save the string
      // in the BufferList with the state encoding.
      const stateEncoding = state.encoding
      value = stateEncoding != undefined ? encoded.toString(stateEncoding) : encoded
    }
  } else if (chunk instanceof Uint8Array) {
    value = Buffer.from(chunk.buffer, chunk.byteOffset, chunk.length)
  } else if (!(isBufferChunk(chunk))) {
    errorOrDestroy(stream, invalidChunk(chunk))
    return false
  }
  if (lengthOf(value) <= 0) return canPushMore(stream, state)
  return readableAddChunkUnshiftValue(stream, state, value)
}

function readableAddChunkUnshiftObjectMode(stream: Stream, state: ReadableState,
                                           chunk: unknown): boolean {
  if (chunk == undefined) {
    state.bits &= ~kReading
    onEofChunk(stream, state)
    return false
  }
  return readableAddChunkUnshiftValue(stream, state, chunk)
}

function readableAddChunkUnshiftValue(stream: Stream, state: ReadableState,
                                      chunk: unknown): boolean {
  if ((state.bits & kEndEmitted) != 0)
    errorOrDestroy(
      stream,
      new StreamError("ERR_STREAM_UNSHIFT_AFTER_END_EVENT", "stream.unshift() after end event"),
    )
  else if ((state.bits & (kDestroyed | kErrored)) != 0) return false
  else addChunk(stream, state, chunk, true)
  return canPushMore(stream, state)
}

// Whether an unknown holds a Buffer.
function isBufferChunk(chunk: unknown): boolean {
  return __tz_isChunk<Buffer>(chunk)
}

function readableAddChunkPushByteMode(stream: Stream, state: ReadableState, chunk: unknown,
                                      encoding: string | undefined): boolean {
  if (chunk == undefined) {
    state.bits &= ~kReading
    onEofChunk(stream, state)
    return false
  }
  let value = chunk
  let enc = encoding ?? ""
  if (typeof chunk == "string") {
    if (enc == "") enc = state.defaultEncoding
    if (state.encoding != normalizeEncoding(enc)) {
      value = Buffer.from(chunk, enc)
      enc = ""
    }
  } else if (isBufferChunk(chunk)) {
    enc = ""
  } else if (chunk instanceof Uint8Array) {
    value = Buffer.from(chunk.buffer, chunk.byteOffset, chunk.length)
    enc = ""
  } else {
    errorOrDestroy(stream, invalidChunk(chunk))
    return false
  }
  if (lengthOf(value) <= 0) {
    state.bits &= ~kReading
    maybeReadMore(stream, state)
    return canPushMore(stream, state)
  }
  if ((state.bits & kEnded) != 0) {
    errorOrDestroy(stream, new StreamError("ERR_STREAM_PUSH_AFTER_EOF", "stream.push() after EOF"))
    return false
  }
  if ((state.bits & (kDestroyed | kErrored)) != 0) return false
  state.bits &= ~kReading
  const decoder = state.decoder
  if (decoder != undefined && enc == "") {
    const text = decoder.write(bufferOf(value))
    if (text.length == 0) {
      maybeReadMore(stream, state)
      return canPushMore(stream, state)
    }
    value = text
  }
  addChunk(stream, state, value, false)
  return canPushMore(stream, state)
}

function readableAddChunkPushObjectMode(stream: Stream, state: ReadableState, chunk: unknown,
                                        encoding: string | undefined): boolean {
  if (chunk == undefined) {
    state.bits &= ~kReading
    onEofChunk(stream, state)
    return false
  }
  if ((state.bits & kEnded) != 0) {
    errorOrDestroy(stream, new StreamError("ERR_STREAM_PUSH_AFTER_EOF", "stream.push() after EOF"))
    return false
  }
  if ((state.bits & (kDestroyed | kErrored)) != 0) return false
  state.bits &= ~kReading
  let value = chunk
  const decoder = state.decoder
  if (decoder != undefined && (encoding == undefined || encoding == "") && isBufferChunk(chunk))
    value = decoder.write(bufferOf(chunk))
  addChunk(stream, state, value, false)
  return canPushMore(stream, state)
}

function canPushMore(stream: Stream, state: ReadableState): boolean {
  // We can push more data if we are below the highWaterMark. Also, if we
  // have no data yet, we can stand some more bytes. This is to work around
  // cases where hwm=0, such as the repl.
  return (state.bits & kEnded) == 0 &&
    (stream.readableLength < stream.readableHighWaterMark || stream.readableLength == 0)
}

function clearAwaitDrain(state: ReadableState) {
  if ((state.bits & kMultiAwaitDrain) != 0) state.awaitDrainWriters.clear()
  else state.awaitDrainWriter = undefined
}

function addChunk(stream: Stream, state: ReadableState, chunk: unknown, addToFront: boolean) {
  if ((state.bits & (kFlowing | kSync | kDataListening)) == (kFlowing | kDataListening) &&
      stream.readableLength == 0) {
    // Use the guard to avoid creating `Set()` repeatedly when we have
    // multiple pipes.
    clearAwaitDrain(state)
    state.bits |= kDataEmitted
    sync(stream)
    stream._emitData(chunk)
  } else {
    // Update the buffer info.
    stream.readableLength += (state.bits & kObjectMode) != 0 ? 1 : lengthOf(chunk)
    if (addToFront) {
      const at = state.bufferIndex - 1
      const buffer = state.buffer
      if (at >= 0 && at < buffer.length) {
        buffer[at] = chunk
        state.bufferIndex = at
      } else {
        state.buffer.unshift(chunk) // Slow path
      }
    } else {
      state.buffer.push(chunk)
    }
    if ((state.bits & kNeedReadable) != 0) emitReadable(stream)
  }
  maybeReadMore(stream, state)
}

// Don't raise the hwm > 1GB.
const kMaxHwm = 0x40000000
function computeNewHighWaterMark(size: int): int {
  if (size > kMaxHwm)
    throw new StreamRangeError(
      "ERR_OUT_OF_RANGE",
      `The value of "size" is out of range. It must be <= 1GiB. Received ${size}`,
    )
  // Get the next highest power of 2 to prevent increasing hwm excessively in
  // tiny amounts.
  let n = size - 1
  n |= n >>> 1
  n |= n >>> 2
  n |= n >>> 4
  n |= n >>> 8
  n |= n >>> 16
  return n + 1
}

// How much to read of `n`, -1 for node's NaN (as much as there is).
function howMuchToRead(stream: Stream, n: int, state: ReadableState): int {
  if ((n <= 0 && n != -1) || (stream.readableLength == 0 && (state.bits & kEnded) != 0)) return 0
  if ((state.bits & kObjectMode) != 0) return 1
  if (n == -1) {
    // Only flow one buffer at a time.
    if ((state.bits & kFlowing) != 0 && stream.readableLength != 0)
      return lengthOf(state.buffer[state.bufferIndex])
    return stream.readableLength
  }
  if (n <= stream.readableLength) return n
  return (state.bits & kEnded) != 0 ? stream.readableLength : 0
}

// You can override either this method, or the async _read(n) below.
function read(stream: Stream, state: ReadableState, size?: int): unknown {
  let n = size ?? -1
  const nOrig = n
  // If we're asking for more than the current hwm, then raise the hwm.
  if (n > stream.readableHighWaterMark) stream.readableHighWaterMark = computeNewHighWaterMark(n)
  if (n != 0) state.bits &= ~kEmittedReadable
  // If we're doing read(0) to trigger a readable event, but we already have
  // a bunch of data in the buffer, then just trigger the 'readable' event
  // and move on.
  if (n == 0 && (state.bits & kNeedReadable) != 0 &&
      ((stream.readableHighWaterMark != 0
        ? stream.readableLength >= stream.readableHighWaterMark
        : stream.readableLength > 0) ||
        (state.bits & kEnded) != 0)) {
    if (stream.readableLength == 0 && (state.bits & kEnded) != 0) endReadable(stream)
    else emitReadable(stream)
    return undefined
  }
  n = howMuchToRead(stream, n, state)
  // If we've ended, and we're now clear, then finish it up.
  if (n == 0 && (state.bits & kEnded) != 0) {
    if (stream.readableLength == 0) endReadable(stream)
    return undefined
  }
  // All the actual chunk generation logic needs to be *below* the call to
  // _read. The reason is that in certain synthetic stream cases, such as
  // passthrough streams, _read may be a completely synchronous operation
  // which may change the state of the read buffer, providing enough data
  // when before there was *not* enough.
  //
  // So, the steps are:
  // 1. Figure out what the state of things will be after we do a read from
  //    the buffer.
  // 2. If that resulting state will trigger a _read, then call _read. Note
  //    that this may be asynchronous, or synchronous. Yes, it is deeply ugly
  //    to write APIs this way, but that still doesn't mean that the Readable
  //    class should behave improperly, as streams are designed to be
  //    sync/async agnostic. Take note if the _read call is sync or async (ie,
  //    if the read call has returned yet), so that we know whether or not
  //    it's safe to emit 'readable' etc.
  // 3. Actually pull the requested chunks out of the buffer and return.

  // if we need a readable event, then we need to do some reading.
  let doRead = (state.bits & kNeedReadable) != 0
  // If we currently have less than the highWaterMark, then also read some.
  if (stream.readableLength == 0 ||
      stream.readableLength - (n == -1 ? 0 : n) < stream.readableHighWaterMark)
    doRead = true
  // However, if we've ended, then there's no point, if we're already
  // reading, then it's unnecessary, if we're constructing we have to wait,
  // and if we're destroyed or errored, then it's not allowed,
  if ((state.bits & (kReading | kEnded | kDestroyed | kErrored | kConstructed)) != kConstructed) {
    doRead = false
  } else if (doRead) {
    state.bits |= kReading | kSync
    // If the length is currently zero, then we *need* a readable event.
    if (stream.readableLength == 0) state.bits |= kNeedReadable
    // Call internal read method
    try {
      stream._read(stream.readableHighWaterMark)
    } catch (err) {
      errorOrDestroy(stream, err)
    }
    state.bits &= ~kSync
    // If _read pushed data synchronously, then `reading` will be false, and
    // we need to re-evaluate how much data we can return to the user.
    if ((state.bits & kReading) == 0) n = howMuchToRead(stream, nOrig, state)
  }
  let ret: unknown = undefined
  if (n > 0) ret = fromList(stream, n, state)
  if (ret == undefined) {
    state.bits |= stream.readableLength <= stream.readableHighWaterMark ? kNeedReadable : 0
    n = 0
  } else {
    stream.readableLength -= n
    clearAwaitDrain(state)
  }
  if (stream.readableLength == 0) {
    // If we have nothing in the buffer, then we want to know as soon as we
    // *do* get something into the buffer.
    if ((state.bits & kEnded) == 0) state.bits |= kNeedReadable
    // If we tried to read() past the EOF, then emit end on the next tick.
    if (nOrig != n && (state.bits & kEnded) != 0) endReadable(stream)
  }
  if (ret != undefined && (state.bits & (kErrorEmitted | kCloseEmitted)) == 0) {
    state.bits |= kDataEmitted
    sync(stream)
    stream._emitData(ret)
  }
  return ret
}

function onEofChunk(stream: Stream, state: ReadableState) {
  if ((state.bits & kEnded) != 0) return
  const decoder = state.decoder
  if (decoder != undefined) {
    const chunk = decoder.end()
    if (chunk.length != 0) {
      state.buffer.push(chunk)
      stream.readableLength += (state.bits & kObjectMode) != 0 ? 1 : chunk.length
    }
  }
  state.bits |= kEnded
  if ((state.bits & kSync) != 0) {
    // If we are sync, wait until next tick to emit the data. Otherwise we risk
    // emitting data in the flow() the readable code triggers during a read()
    // call.
    emitReadable(stream)
  } else {
    // Emit 'readable' now to make sure it gets picked up.
    state.bits &= ~kNeedReadable
    state.bits |= kEmittedReadable
    // We have to emit readable now that we are EOF. Modules in the ecosystem
    // (e.g. dicer) rely on this event being sync.
    emitReadableNow(stream)
  }
}

// Don't emit readable right away in sync mode, because this can trigger
// another read() call => stack overflow. This way, it might trigger a
// nextTick recursion warning, but that's not so bad.
function emitReadable(stream: Stream) {
  const state = readableStateOf(stream)
  state.bits &= ~kNeedReadable
  if ((state.bits & kEmittedReadable) == 0) {
    state.bits |= kEmittedReadable
    process.nextTick(() => emitReadableNow(stream))
  }
}

// node's emitReadable_.
function emitReadableNow(stream: Stream) {
  const state = readableStateOf(stream)
  if ((state.bits & (kDestroyed | kErrored)) == 0 &&
      (stream.readableLength != 0 || (state.bits & kEnded) != 0)) {
    sync(stream)
    stream.emit("readable")
    state.bits &= ~kEmittedReadable
  }
  // The stream needs another readable event if:
  // 1. It is not flowing, as the flow mechanism will take care of it.
  // 2. It is not ended.
  // 3. It is below the highWaterMark, so we can schedule another readable
  //    later.
  state.bits |=
    (state.bits & (kFlowing | kEnded)) == 0 &&
      stream.readableLength <= stream.readableHighWaterMark
      ? kNeedReadable
      : 0
  flow(stream)
}

// At this point, the user has presumably seen the 'readable' event, and
// called read() to consume some data. that may have triggered in turn
// another _read(n) call, in which case reading = true if it's in progress.
// However, if we're not ended, or reading, and the length < hwm, then go
// ahead and try to read some more preemptively.
function maybeReadMore(stream: Stream, state: ReadableState) {
  if ((state.bits & (kReadingMore | kReading | kConstructed)) == kConstructed) {
    state.bits |= kReadingMore
    process.nextTick(() => maybeReadMoreNow(stream, state))
  }
}

function maybeReadMoreNow(stream: Stream, state: ReadableState) {
  // Attempt to read more data if we should.
  //
  // The conditions for reading more data are (one of):
  // - Not enough data buffered (state.length < state.highWaterMark). The
  //   loop is responsible for filling the buffer with enough data if such
  //   data is available. If highWaterMark is 0 and we are not in the flowing
  //   mode we should _not_ attempt to buffer any extra data. We'll get more
  //   data when the stream consumer calls read() instead.
  // - No data in the buffer, and the stream is in flowing mode. In this mode
  //   the loop below is responsible for ensuring read() is called. Failing
  //   to call read here would abort the flow and there's no other mechanism
  //   for continuing the flow if the stream consumer has just subscribed to
  //   the 'data' event.
  //
  // In addition to the above conditions to keep reading data, the following
  // conditions prevent the data from being read:
  // - The stream has ended (state.ended).
  // - There is already a pending 'read' operation (state.reading). This is a
  //   case where the stream has called the implementation defined _read()
  //   method, but they are processing the call asynchronously and have _not_
  //   called push() with new data. In this case we skip performing more
  //   read()s. The execution ends in this method again after the _read()
  //   ends up calling push() with more data.
  while ((state.bits & (kReading | kEnded)) == 0 &&
         (stream.readableLength < stream.readableHighWaterMark ||
          ((state.bits & kFlowing) != 0 && stream.readableLength == 0))) {
    const len = stream.readableLength
    read(stream, state, 0)
    // Didn't get any data, stop spinning.
    if (len == stream.readableLength) break
  }
  state.bits &= ~kReadingMore
  sync(stream)
}

function updateReadableListening(self: Stream) {
  const state = readableStateOf(self)
  if (self.listenerCount("readable") > 0) state.bits |= kReadableListening
  else state.bits &= ~kReadableListening
  if ((state.bits & (kHasPaused | kPaused | kResumeScheduled)) == (kHasPaused | kResumeScheduled)) {
    // Flowing needs to be set to true now, otherwise the upcoming resume will
    // not flow.
    state.bits |= kHasFlowing | kFlowing
    // Crude way to check if we should resume.
  } else if ((state.bits & kDataListening) != 0) {
    self.resume()
  } else if ((state.bits & kReadableListening) == 0) {
    state.bits &= ~(kHasFlowing | kFlowing)
  }
  sync(self)
}

function resume(stream: Stream, state: ReadableState) {
  if ((state.bits & kResumeScheduled) == 0) {
    state.bits |= kResumeScheduled
    process.nextTick(() => resumeNow(stream, state))
  }
}

function resumeNow(stream: Stream, state: ReadableState) {
  if ((state.bits & kReading) == 0) read(stream, state, 0)
  state.bits &= ~kResumeScheduled
  sync(stream)
  stream.emit("resume")
  flow(stream)
  if ((state.bits & (kFlowing | kReading)) == kFlowing) read(stream, state, 0)
  sync(stream)
}

function flow(stream: Stream) {
  const state = readableStateOf(stream)
  while ((state.bits & kFlowing) != 0 && read(stream, state) != undefined) {}
}

// The chunk at `idx` of a buffer, which a read takes, or what is left of it.
function setChunk(buf: unknown[], idx: int, chunk: unknown) {
  if (idx >= 0 && idx < buf.length) buf[idx] = chunk
}

// Pluck off n bytes from an array of buffers. Length is the combined lengths
// of all the buffers in the list.
function fromList(stream: Stream, size: int, state: ReadableState): unknown {
  // nothing buffered.
  if (stream.readableLength == 0) return undefined
  let n = size
  let idx = state.bufferIndex
  let ret: unknown
  const buf = state.buffer
  const len = buf.length
  if ((state.bits & kObjectMode) != 0) {
    ret = buf[idx]
    setChunk(buf, idx++, undefined)
  } else if (n == 0 || n >= stream.readableLength) {
    // Read it all, truncate the list.
    if ((state.bits & kDecoder) != 0) {
      let text = ""
      while (idx < len) {
        text += textOf(buf[idx])
        setChunk(buf, idx++, undefined)
      }
      ret = text
    } else if (len - idx == 0) {
      ret = Buffer.alloc(0)
    } else if (len - idx == 1) {
      ret = buf[idx]
      setChunk(buf, idx++, undefined)
    } else {
      const out = Buffer.allocUnsafe(stream.readableLength)
      let i = 0
      while (idx < len) {
        const data = bufferOf(buf[idx])
        out.set(data, i)
        i += data.length
        setChunk(buf, idx++, undefined)
      }
      ret = out
    }
  } else if (n < lengthOf(buf[idx])) {
    // `slice` is the same for buffers and strings.
    const first = buf[idx]
    if (typeof first == "string") {
      ret = first.slice(0, n)
      setChunk(buf, idx, first.slice(n))
    } else {
      const data = bufferOf(first)
      ret = data.subarray(0, n)
      setChunk(buf, idx, data.subarray(n))
    }
  } else if (n == lengthOf(buf[idx])) {
    // First chunk is a perfect match.
    ret = buf[idx]
    setChunk(buf, idx++, undefined)
  } else if ((state.bits & kDecoder) != 0) {
    let text = ""
    while (idx < len) {
      const str = textOf(buf[idx])
      if (n > str.length) {
        text += str
        n -= str.length
        setChunk(buf, idx++, undefined)
      } else {
        if (n == buf.length) {
          text += str
          setChunk(buf, idx++, undefined)
        } else {
          text += str.slice(0, n)
          setChunk(buf, idx, str.slice(n))
        }
        break
      }
    }
    ret = text
  } else {
    const out = Buffer.allocUnsafe(n)
    const retLen = n
    while (idx < len) {
      const data = bufferOf(buf[idx])
      if (n > data.length) {
        out.set(data, retLen - n)
        n -= data.length
        setChunk(buf, idx++, undefined)
      } else {
        if (n == data.length) {
          out.set(data, retLen - n)
          setChunk(buf, idx++, undefined)
        } else {
          out.set(data.subarray(0, n), retLen - n)
          setChunk(buf, idx, data.subarray(n))
        }
        break
      }
    }
    ret = out
  }
  if (idx == len) {
    state.buffer = []
    state.bufferIndex = 0
  } else if (idx > 1024) {
    state.buffer.splice(0, idx)
    state.bufferIndex = 0
  } else {
    state.bufferIndex = idx
  }
  return ret
}

function textOf(chunk: unknown): string {
  return typeof chunk == "string" ? chunk : ""
}

function endReadable(stream: Stream) {
  const state = readableStateOf(stream)
  if ((state.bits & kEndEmitted) == 0) {
    state.bits |= kEnded
    process.nextTick(() => endReadableNT(state, stream))
  }
}

function endReadableNT(state: ReadableState, stream: Stream) {
  // Check that we didn't get one last unshift.
  if ((state.bits & (kErrored | kCloseEmitted | kEndEmitted)) == 0 &&
      stream.readableLength == 0) {
    state.bits |= kEndEmitted
    sync(stream)
    stream.emit("end")
    const w = stream._writableState
    if (stream.writable && !stream.allowHalfOpen) {
      process.nextTick(() => endWritableNT(stream))
    } else if ((state.bits & kAutoDestroy) != 0) {
      // In case of duplex streams we need a way to detect if the writable side
      // is ready for autoDestroy as well.
      const autoDestroy =
        w == undefined ||
        ((w.bits & kAutoDestroy) != 0 &&
          // We don't expect the writable to ever 'finish' if writable is
          // explicitly set to false.
          ((w.bits & kFinished) != 0 || w.writable == false))
      if (autoDestroy) stream.destroy()
    }
  }
}

function endWritableNT(stream: Stream) {
  const writable = stream.writable && !stream.writableEnded && !stream.destroyed
  if (writable) stream._endWith(false, undefined, undefined, undefined)
}

// pipe() of node. Its listeners are functions of one block, which share
// one environment and call each other without capturing each other: they
// hold the source and the destination until cleanup() removes them.
function pipe(src: Stream, dest: Stream, end: boolean) {
  const state = readableStateOf(src)
  let ondrain: (() => void) | undefined = undefined
  let cleanedUp = false
  if (state.pipes.length == 1) {
    if ((state.bits & kMultiAwaitDrain) == 0) {
      state.bits |= kMultiAwaitDrain
      state.awaitDrainWriters = new Set<Stream>()
      const writer = state.awaitDrainWriter
      if (writer != undefined) state.awaitDrainWriters.add(writer)
      state.awaitDrainWriter = undefined
    }
  }
  state.pipes.push(dest)
  const doEnd = end && !isStdio(dest)
  if ((state.bits & kEndEmitted) != 0) {
    if (doEnd) process.nextTick(onend)
    else process.nextTick(unpipe)
  } else if (doEnd) {
    src.once("end", onend)
  } else {
    src.once("end", unpipe)
  }
  dest.on("unpipe", onunpipe)
  function onunpipe(readable: Stream, unpipeInfo: { hasUnpiped: boolean }) {
    if (readable == src && !unpipeInfo.hasUnpiped) {
      unpipeInfo.hasUnpiped = true
      cleanup()
    }
  }
  function onend() {
    dest._endWith(false, undefined, undefined, undefined)
  }
  function cleanup() {
    // Cleanup event handlers once the pipe is broken.
    dest.removeListener("close", onclose)
    dest.removeListener("finish", onfinish)
    if (ondrain != undefined) dest.removeListener("drain", ondrain)
    dest.removeListener("error", onerror)
    dest.removeListener("unpipe", onunpipe)
    src.removeListener("end", onend)
    src.removeListener("end", unpipe)
    src.removeListener("data", ondata)
    cleanedUp = true
    // If the reader is waiting for a drain event from this specific writer,
    // then it would cause it to never start flowing again. So, if this is
    // awaiting a drain, then we just call it now. If we don't know, then
    // assume that we are waiting for one.
    const awaiting =
      (state.bits & kMultiAwaitDrain) != 0 || state.awaitDrainWriter != undefined
    const w = dest._writableState
    if (ondrain != undefined && awaiting && (w == undefined || (w.bits & kNeedDrain) != 0))
      ondrain()
  }
  function pause() {
    // If the user unpiped during `dest.write()`, it is possible to get stuck
    // in a permanently paused state if that write also returned false.
    // => Check whether `dest` is still a piping destination.
    if (!cleanedUp) {
      if (state.pipes.length == 1 && state.pipes[0] == dest) {
        state.awaitDrainWriter = dest
        state.bits &= ~kMultiAwaitDrain
      } else if (state.pipes.length > 1 && state.pipes.includes(dest)) {
        state.awaitDrainWriters.add(dest)
      }
      src.pause()
    }
    if (ondrain == undefined) {
      // When the dest drains, it reduces the awaitDrain counter on the
      // source. This would be more elegant with a .once() handler in flow(),
      // but adding and removing repeatedly is too slow.
      const drained = pipeOnDrain(src, dest)
      ondrain = drained
      dest.on("drain", drained)
    }
  }
  // The chunk as an unknown, whatever the type the source emits it with.
  src.on("data", ondata, kUnknownArgListener)
  function ondata(chunk: unknown) {
    try {
      const ret = dest._writeAny(chunk, undefined, nop)
      if (ret != true) pause()
    } catch (error) {
      dest.destroy(error)
    }
  }
  // If the dest has an error, then stop piping into it. However, don't
  // suppress the throwing behavior for this.
  function onerror(er: Error) {
    unpipe()
    dest.removeListener("error", onerror)
    if (dest.listenerCount("error") == 0) {
      const s = dest._writableState
      const r = dest._readableState
      const errorEmitted = s != undefined
        ? (s.bits & kErrorEmitted) != 0
        : r != undefined && (r.bits & kErrorEmitted) != 0
      // User incorrectly emitted 'error' directly on the stream.
      if ((s != undefined || r != undefined) && !errorEmitted) errorOrDestroy(dest, er)
      else dest.emit("error", er)
    }
  }
  // Make sure our error handler is attached before userland ones.
  dest.prependListener("error", onerror)
  // Both close and finish should trigger unpipe, but only once.
  function onclose() {
    dest.removeListener("finish", onfinish)
    unpipe()
  }
  dest.once("close", onclose)
  function onfinish() {
    dest.removeListener("close", onclose)
    unpipe()
  }
  dest.once("finish", onfinish)
  function unpipe() {
    src.unpipe(dest)
  }
  // Tell the dest that it's being piped to.
  dest.emit("pipe", src)
  // Start the flow if it hasn't been started already.
  if (dest.writableNeedDrain) pause()
  else if ((state.bits & kFlowing) == 0) src.resume()
}

function pipeOnDrain(src: Stream, dest: Stream): () => void {
  return () => {
    const state = readableStateOf(src)
    // `ondrain` will call directly, `this` maybe not a reference to dest, so
    // we use the real dest here.
    if ((state.bits & kMultiAwaitDrain) == 0 && state.awaitDrainWriter == dest) {
      state.awaitDrainWriter = undefined
    } else if ((state.bits & kMultiAwaitDrain) != 0) {
      state.awaitDrainWriters.delete(dest)
    }
    const empty = (state.bits & kMultiAwaitDrain) != 0
      ? state.awaitDrainWriters.size == 0
      : state.awaitDrainWriter == undefined
    if (empty && (state.bits & kDataListening) != 0) src.resume()
  }
}

// process.stdout and process.stderr, which pipe() does not end.
let stdioStreams: Stream[] = []

function isStdio(stream: Stream): boolean {
  return stdioStreams.includes(stream)
}

//===----------------------------------------------------------------------===//
// lib/internal/streams/writable.js
//===----------------------------------------------------------------------===//

function resetBuffer(state: WritableState) {
  state.buffered = []
  state.bufferedIndex = 0
  state.bits |= kAllBuffers | kAllNoop
  state.bits &= ~kBuffered
}

// node's _write() of Writable: the checks of write(), then writeOrBuffer().
function writeImpl(stream: Stream, chunk: unknown, encoding: string | undefined,
                   cb: StreamCallback): boolean | Error {
  const state = writableStateOf(stream)
  let value = chunk
  let enc = encoding ?? ""
  if (chunk == undefined) throw nullValues()
  if ((state.bits & kObjectMode) == 0) {
    if (enc == "") enc = state.defaultEncoding
    else if (enc != "buffer" && !Buffer.isEncoding(enc)) throw unknownEncoding(enc)
    if (typeof chunk == "string") {
      if ((state.bits & kDecodeStrings) != 0) {
        value = Buffer.from(chunk, enc)
        enc = "buffer"
      }
    } else if (isBufferChunk(chunk)) {
      enc = "buffer"
    } else if (chunk instanceof Uint8Array) {
      value = Buffer.from(chunk.buffer, chunk.byteOffset, chunk.length)
      enc = "buffer"
    } else {
      throw invalidChunk(chunk)
    }
  }
  let err: Error | undefined = undefined
  if ((state.bits & kEnding) != 0)
    err = new StreamError("ERR_STREAM_WRITE_AFTER_END", "write after end")
  else if ((state.bits & kDestroyed) != 0) err = streamDestroyed("write")
  if (err != undefined) {
    const failed = err
    process.nextTick(() => cb(failed))
    errorOrDestroy(stream, err, true)
    return err
  }
  state.pendingcb++
  return writeOrBuffer(stream, state, value, enc, cb)
}

// If we're already writing something, then just put this in the queue, and
// wait our turn. Otherwise, call _write. If we return false, then we need a
// drain event, so set that flag.
function writeOrBuffer(stream: Stream, state: WritableState, chunk: unknown, encoding: string,
                       callback: StreamCallback): boolean {
  const len = (state.bits & kObjectMode) != 0 ? 1 : lengthOf(chunk)
  stream.writableLength += len
  if ((state.bits & (kWriting | kErrored | kCorked | kConstructed)) != kConstructed) {
    if ((state.bits & kBuffered) == 0) {
      state.bits |= kBuffered
      state.buffered = []
    }
    state.buffered.push({ chunk, encoding, callback })
    if ((state.bits & kAllBuffers) != 0 && encoding != "buffer") state.bits &= ~kAllBuffers
    if ((state.bits & kAllNoop) != 0 && callback != nop) state.bits &= ~kAllNoop
  } else {
    state.writelen = len
    if (callback != nop) state.writecb = callback
    state.bits |= kWriting | kWSync | kExpectWriteCb
    stream._writeChunk(chunk, encoding, stream._onwrite)
    state.bits &= ~kWSync
  }
  const ret = stream.writableLength < stream.writableHighWaterMark || stream.writableLength == 0
  if (!ret) state.bits |= kNeedDrain
  // Return false if errored or destroyed in order to break any synchronous
  // while(stream.write(data)) loops.
  return ret && (state.bits & (kDestroyed | kErrored)) == 0
}

function doWrite(stream: Stream, state: WritableState, len: int, chunk: unknown,
                 encoding: string, cb: StreamCallback) {
  state.writelen = len
  if (cb != nop) state.writecb = cb
  state.bits |= kWriting | kWSync | kExpectWriteCb
  if ((state.bits & kDestroyed) != 0) onwrite(stream, streamDestroyed("write"))
  else stream._writeChunk(chunk, encoding, stream._onwrite)
  state.bits &= ~kWSync
}

function doWritev(stream: Stream, state: WritableState, len: int, chunks: BufferedWrite[],
                  cb: StreamCallback) {
  state.writelen = len
  if (cb != nop) state.writecb = cb
  state.bits |= kWriting | kWSync | kExpectWriteCb
  if ((state.bits & kDestroyed) != 0) onwrite(stream, streamDestroyed("write"))
  else stream._writevChunks(chunks, stream._onwrite)
  state.bits &= ~kWSync
}

function onwriteError(stream: Stream, state: WritableState, er: Error, cb: StreamCallback) {
  --state.pendingcb
  cb(er)
  // Ensure callbacks are invoked even when autoDestroy is not enabled.
  // Passing `er` here doesn't make sense since it's related to one specific
  // write, not to the buffered writes.
  errorBuffer(stream, state)
  // This can emit error, but error must always follow cb.
  errorOrDestroy(stream, er)
}

function onwrite(stream: Stream, er: Error | undefined) {
  const state = writableStateOf(stream)
  if ((state.bits & kExpectWriteCb) == 0) {
    errorOrDestroy(stream, multipleCallback())
    return
  }
  const isSync = (state.bits & kWSync) != 0
  const cb = state.writecb
  state.writecb = nop
  state.bits &= ~(kWriting | kExpectWriteCb)
  stream.writableLength -= state.writelen
  state.writelen = 0
  if (er != undefined) {
    if ((state.bits & kErrored) == 0) {
      state.errored = er
      state.bits |= kErrored
    }
    // In case of duplex streams we need to notify the readable side of the
    // error.
    const r = stream._readableState
    if (r != undefined && r.errored == undefined) {
      r.errored = er
      r.bits |= kErrored
    }
    sync(stream)
    const error = er
    if (isSync) process.nextTick(() => onwriteError(stream, state, error, cb))
    else onwriteError(stream, state, error, cb)
  } else {
    if ((state.bits & kBuffered) != 0) clearBuffer(stream, state)
    if (isSync) {
      const needDrain = (state.bits & kNeedDrain) != 0 && stream.writableLength == 0
      const needTick = needDrain || (state.bits & kDestroyed) != 0 || cb != nop
      // It is a common case that the callback passed to .write() is always
      // the same. In that case, we do not schedule a new nextTick(), but
      // rather just increase a counter, to improve performance and avoid
      // memory allocations.
      const info = state.afterWriteTickInfo
      if (cb == nop) {
        if ((state.bits & kAfterWritePending) == 0 && needTick) {
          process.nextTick(() => afterWrite(stream, state, 1, cb))
          state.bits |= kAfterWritePending
        } else {
          state.pendingcb--
          if ((state.bits & kEnding) != 0) finishMaybe(stream, state, true)
        }
      } else if ((state.bits & kAfterWriteTickInfo) != 0 && info != undefined && info.cb == cb) {
        info.count++
      } else if (needTick) {
        const tick = new AfterWriteTick(cb)
        state.afterWriteTickInfo = tick
        process.nextTick(() => afterWriteTick(stream, state, tick))
        state.bits |= kAfterWritePending | kAfterWriteTickInfo
      } else {
        state.pendingcb--
        if ((state.bits & kEnding) != 0) finishMaybe(stream, state, true)
      }
    } else {
      afterWrite(stream, state, 1, cb)
    }
  }
  sync(stream)
}

function afterWriteTick(stream: Stream, state: WritableState, tick: AfterWriteTick) {
  state.bits &= ~kAfterWriteTickInfo
  state.afterWriteTickInfo = undefined
  afterWrite(stream, state, tick.count, tick.cb)
}

function afterWrite(stream: Stream, state: WritableState, count: int, cb: StreamCallback) {
  state.bits &= ~kAfterWritePending
  const needDrain = (state.bits & (kEnding | kNeedDrain | kDestroyed)) == kNeedDrain &&
    stream.writableLength == 0
  if (needDrain) {
    state.bits &= ~kNeedDrain
    sync(stream)
    stream.emit("drain")
  }
  let left = count
  while (left-- > 0) {
    state.pendingcb--
    cb()
  }
  if ((state.bits & kDestroyed) != 0) errorBuffer(stream, state)
  if ((state.bits & kEnding) != 0) finishMaybe(stream, state, true)
  sync(stream)
}

// If there's something in the buffer waiting, then invoke callbacks.
function errorBuffer(stream: Stream, state: WritableState) {
  if ((state.bits & kWriting) != 0) return
  if ((state.bits & kBuffered) != 0) {
    for (let n = state.bufferedIndex; n < state.buffered.length; ++n) {
      const entry = state.buffered[n]
      if (entry == undefined) continue
      const len = (state.bits & kObjectMode) != 0 ? 1 : lengthOf(entry.chunk)
      stream.writableLength -= len
      entry.callback(state.errored ?? streamDestroyed("write"))
    }
  }
  callFinishedCallbacks(state, state.errored ?? streamDestroyed("end"))
  resetBuffer(state)
  sync(stream)
}

// If there's something in the buffer waiting, then process it.
function clearBuffer(stream: Stream, state: WritableState) {
  if ((state.bits & (kDestroyed | kBufferProcessing | kCorked | kBuffered | kConstructed)) !=
      (kBuffered | kConstructed))
    return
  const objectMode = (state.bits & kObjectMode) != 0
  const buffered = state.buffered
  const bufferedIndex = state.bufferedIndex
  const bufferedLength = buffered.length - bufferedIndex
  if (bufferedLength == 0) return
  let i = bufferedIndex
  state.bits |= kBufferProcessing
  if (bufferedLength > 1 && stream._hasWritev) {
    state.pendingcb -= bufferedLength - 1
    const start = i
    const callback: StreamCallback = (state.bits & kAllNoop) != 0
      ? nop
      : (err?: Error) => {
          for (let n = start; n < buffered.length; ++n) {
            const entry = buffered[n]
            if (entry != undefined) entry.callback(err)
          }
        }
    const chunks: BufferedWrite[] = []
    for (let n = i; n < buffered.length; n++) {
      const entry = buffered[n]
      if (entry != undefined) chunks.push(entry)
    }
    doWritev(stream, state, stream.writableLength, chunks, callback)
    resetBuffer(state)
  } else {
    do {
      const entry = buffered[i]
      if (i >= 0 && i < buffered.length) buffered[i] = undefined
      i++
      if (entry == undefined) continue
      const len = objectMode ? 1 : lengthOf(entry.chunk)
      doWrite(stream, state, len, entry.chunk, entry.encoding, entry.callback)
    } while (i < buffered.length && (state.bits & kWriting) == 0)
    if (i == buffered.length) {
      resetBuffer(state)
    } else if (i > 256) {
      buffered.splice(0, i)
      state.bufferedIndex = 0
    } else {
      state.bufferedIndex = i
    }
  }
  state.bits &= ~kBufferProcessing
}

function needFinish(stream: Stream, state: WritableState): boolean {
  // State is ended && constructed but not destroyed, finished, writing,
  // errorEmitted or closedEmitted.
  return (state.bits & (kEnding | kDestroyed | kConstructed | kFinished | kWriting |
    kErrorEmitted | kCloseEmitted | kErrored | kBuffered)) == (kEnding | kConstructed) &&
    stream.writableLength == 0
}

function onFinish(stream: Stream, state: WritableState, err: Error | undefined) {
  if ((state.bits & kPrefinished) != 0) {
    errorOrDestroy(stream, err ?? multipleCallback())
    return
  }
  state.pendingcb--
  if (err != undefined) {
    callFinishedCallbacks(state, err)
    errorOrDestroy(stream, err, (state.bits & kWSync) != 0)
  } else if (needFinish(stream, state)) {
    state.bits |= kPrefinished
    sync(stream)
    stream.emit("prefinish")
    // Backwards compat. Don't check state.sync here. Some streams assume
    // 'finish' will be emitted asynchronously relative to _final callback.
    state.pendingcb++
    process.nextTick(() => finish(stream, state))
  }
}

function prefinish(stream: Stream, state: WritableState) {
  if ((state.bits & (kPrefinished | kFinalCalled)) != 0) return
  if (stream._hasFinal && (state.bits & kDestroyed) == 0) {
    state.bits |= kFinalCalled | kWSync
    state.pendingcb++
    try {
      stream._final((err?: Error) => onFinish(stream, state, err))
    } catch (err) {
      onFinish(stream, state, err)
    }
    state.bits &= ~kWSync
  } else {
    state.bits |= kFinalCalled | kPrefinished
    sync(stream)
    stream.emit("prefinish")
  }
}

function finishMaybe(stream: Stream, state: WritableState, isSync: boolean) {
  if (needFinish(stream, state)) {
    prefinish(stream, state)
    if (state.pendingcb == 0) {
      if (isSync) {
        state.pendingcb++
        process.nextTick(() => {
          if (needFinish(stream, state)) finish(stream, state)
          else state.pendingcb--
        })
      } else if (needFinish(stream, state)) {
        state.pendingcb++
        finish(stream, state)
      }
    }
  }
}

function finish(stream: Stream, state: WritableState) {
  state.pendingcb--
  state.bits |= kFinished
  callFinishedCallbacks(state, undefined)
  sync(stream)
  stream.emit("finish")
  if ((state.bits & kAutoDestroy) != 0) {
    // In case of duplex streams we need a way to detect if the readable side
    // is ready for autoDestroy as well.
    const r = stream._readableState
    const autoDestroy =
      r == undefined ||
      ((r.bits & kAutoDestroy) != 0 &&
        // We don't expect the readable to ever 'end' if readable is
        // explicitly set to false.
        ((r.bits & kEndEmitted) != 0 || r.readable == false))
    if (autoDestroy) stream.destroy()
  }
}

function callFinishedCallbacks(state: WritableState, err: Error | undefined) {
  if ((state.bits & kOnFinished) == 0) return
  const onfinishCallbacks = state.onFinished
  state.onFinished = []
  state.bits &= ~kOnFinished
  for (const cb of onfinishCallbacks) cb(err)
}

//===----------------------------------------------------------------------===//
// lib/internal/streams/destroy.js
//===----------------------------------------------------------------------===//

function checkError(stream: Stream, err: Error | undefined) {
  if (err == undefined) return
  const w = stream._writableState
  const r = stream._readableState
  if (w != undefined && w.errored == undefined) {
    w.errored = err
    w.bits |= kErrored
  }
  if (r != undefined && r.errored == undefined) {
    r.errored = err
    r.bits |= kErrored
  }
}

// destroy() of node.
function destroyImpl(stream: Stream, err: Error | undefined, cb: StreamCallback | undefined) {
  const r = stream._readableState
  const w = stream._writableState
  if ((w != undefined && (w.bits & kDestroyed) != 0) ||
      (r != undefined && (r.bits & kDestroyed) != 0)) {
    if (cb != undefined) cb()
    return
  }
  // We set destroyed to true before firing error callbacks in order to make
  // it re-entrance safe in case destroy() is called within callbacks
  checkError(stream, err)
  if (w != undefined) w.bits |= kDestroyed
  if (r != undefined) r.bits |= kDestroyed
  sync(stream)
  // If still constructing then defer calling _destroy.
  const s = w ?? r
  if (s != undefined && (s.bits & kConstructed) == 0) {
    stream._onDestroy.push((er: Error | undefined) =>
      destroyNow(stream, aggregateTwoErrors(er, err), cb))
  } else {
    destroyNow(stream, err, cb)
  }
}

// node's _destroy().
function destroyNow(self: Stream, err: Error | undefined, cb: StreamCallback | undefined) {
  let called = false
  function onDestroy(error?: Error) {
    if (called) return
    called = true
    checkError(self, error)
    const w = self._writableState
    const r = self._readableState
    if (w != undefined) w.bits |= kClosed
    if (r != undefined) r.bits |= kClosed
    sync(self)
    if (cb != undefined) cb(error)
    if (error != undefined) {
      const failed = error
      process.nextTick(() => {
        emitErrorNT(self, failed)
        emitCloseNT(self)
      })
    } else {
      process.nextTick(() => emitCloseNT(self))
    }
  }
  try {
    self._destroy(err, onDestroy)
  } catch (error) {
    onDestroy(error)
  }
}

function emitCloseNT(self: Stream) {
  const r = self._readableState
  const w = self._writableState
  if (w != undefined) w.bits |= kCloseEmitted
  if (r != undefined) r.bits |= kCloseEmitted
  sync(self)
  if ((w != undefined && (w.bits & kEmitClose) != 0) ||
      (r != undefined && (r.bits & kEmitClose) != 0))
    self.emit("close")
}

function emitErrorNT(self: Stream, err: Error) {
  const r = self._readableState
  const w = self._writableState
  if ((w != undefined && (w.bits & kErrorEmitted) != 0) ||
      (r != undefined && (r.bits & kErrorEmitted) != 0))
    return
  if (w != undefined) w.bits |= kErrorEmitted
  if (r != undefined) r.bits |= kErrorEmitted
  sync(self)
  self.emit("error", err)
}

function errorOrDestroy(stream: Stream, err: Error, isSync = false) {
  // We have tests that rely on errors being emitted in the same tick, so
  // changing this is semver major. For now when you opt-in to autoDestroy we
  // allow us to fix this and the error will be emitted on the next tick.
  const r = stream._readableState
  const w = stream._writableState
  if ((w != undefined && (w.bits & kDestroyed) != 0) ||
      (r != undefined && (r.bits & kDestroyed) != 0))
    return
  if ((r != undefined && (r.bits & kAutoDestroy) != 0) ||
      (w != undefined && (w.bits & kAutoDestroy) != 0)) {
    stream.destroy(err)
  } else {
    if (w != undefined && (w.bits & kErrored) == 0) {
      w.errored = err
      w.bits |= kErrored
    }
    if (r != undefined && (r.bits & kErrored) == 0) {
      r.errored = err
      r.bits |= kErrored
    }
    sync(stream)
    if (isSync) process.nextTick(() => emitErrorNT(stream, err))
    else emitErrorNT(stream, err)
  }
}

function construct(stream: Stream, cb: () => void) {
  const r = stream._readableState
  const w = stream._writableState
  if (r != undefined) r.bits &= ~kConstructed
  if (w != undefined) w.bits &= ~kConstructed
  stream._onConstruct.push(cb)
  if (stream._onConstruct.length > 1) return
  process.nextTick(() => constructNT(stream))
}

function constructNT(stream: Stream) {
  let called = false
  function onConstruct(err?: Error) {
    if (called) {
      errorOrDestroy(stream, err ?? multipleCallback())
      return
    }
    called = true
    const r = stream._readableState
    const w = stream._writableState
    const s = w ?? r
    if (r != undefined) r.bits |= kConstructed
    if (w != undefined) w.bits |= kConstructed
    // What waits for the construction holds the stream: it goes, whichever
    // way the construction ends, as node's listeners of kConstruct do with
    // the stream.
    const constructed = stream._onConstruct
    stream._onConstruct = []
    if (s != undefined && (s.bits & kDestroyed) != 0) {
      const waits = stream._onDestroy
      stream._onDestroy = []
      for (const wait of waits) wait(err)
    } else if (err != undefined) {
      errorOrDestroy(stream, err, true)
    } else {
      for (const wait of constructed) wait()
    }
    sync(stream)
  }
  try {
    stream._construct((err?: Error) => {
      process.nextTick(() => onConstruct(err))
    })
  } catch (err) {
    process.nextTick(() => onConstruct(err))
  }
}

// destroyer() of node: destroys a stream the way it can be.
function destroyer(stream: Stream, err: Error | undefined) {
  if (isDestroyed(stream)) return
  let error = err
  if (error == undefined && !isFinished(stream)) error = new AbortError()
  stream.destroy(error)
}

//===----------------------------------------------------------------------===//
// lib/internal/streams/utils.js
//===----------------------------------------------------------------------===//

function isDestroyed(stream: Stream): boolean {
  const state = stream._writableState ?? stream._readableState
  return stream.destroyed || (state != undefined && (state.bits & kDestroyed) != 0)
}

function isReadableNodeStream(stream: Stream): boolean {
  const r = stream._readableState
  return r != undefined && (stream._writableState == undefined || r.readable != false)
}

function isWritableNodeStream(stream: Stream): boolean {
  const w = stream._writableState
  return w != undefined && (stream._readableState == undefined || w.writable != false)
}

function isWritableEnded(stream: Stream): boolean {
  if (stream.writableEnded) return true
  const w = stream._writableState
  if (w == undefined || w.errored != undefined) return false
  return (w.bits & kWEnded) != 0
}

function isWritableFinished(stream: Stream, strict: boolean): boolean {
  if (!isWritableNodeStream(stream)) return false
  if (stream.writableFinished) return true
  const w = writableStateOf(stream)
  if (w.errored != undefined) return false
  return (w.bits & kFinished) != 0 ||
    (!strict && (w.bits & kWEnded) != 0 && stream.writableLength == 0)
}

function isReadableFinished(stream: Stream, strict: boolean): boolean {
  if (!isReadableNodeStream(stream)) return false
  const r = readableStateOf(stream)
  if (r.errored != undefined) return false
  return (r.bits & kEndEmitted) != 0 ||
    (!strict && (r.bits & kEnded) != 0 && stream.readableLength == 0)
}

function isReadable(stream: Stream): boolean {
  if (isDestroyed(stream)) return false
  return isReadableNodeStream(stream) && stream.readable && !isReadableFinished(stream, true)
}

function isWritable(stream: Stream): boolean {
  if (isDestroyed(stream)) return false
  return isWritableNodeStream(stream) && stream.writable && !isWritableEnded(stream)
}

function isFinished(stream: Stream): boolean {
  if (isDestroyed(stream)) return true
  if (isReadable(stream)) return false
  if (isWritable(stream)) return false
  return true
}

function isClosed(stream: Stream): boolean {
  const w = stream._writableState
  const r = stream._readableState
  return (w != undefined && (w.bits & kClosed) != 0) || (r != undefined && (r.bits & kClosed) != 0)
}

function willEmitClose(stream: Stream): boolean {
  const state = stream._writableState ?? stream._readableState
  return state != undefined && (state.bits & kAutoDestroy) != 0 &&
    (state.bits & kEmitClose) != 0 && (state.bits & kClosed) == 0
}

function erroredOf(stream: Stream): Error | undefined {
  return stream._writableState?.errored ?? stream._readableState?.errored
}

// Stream.isDestroyed() and the others of node:stream.
export function isErrored(stream: Stream): boolean {
  const r = stream._readableState
  const w = stream._writableState
  return (r != undefined && (r.bits & kErrorEmitted) != 0) ||
    (w != undefined && (w.bits & kErrorEmitted) != 0) || erroredOf(stream) != undefined
}

export function isDisturbed(stream: Stream): boolean {
  return stream.readableDidRead || stream.readableAborted
}

//===----------------------------------------------------------------------===//
// lib/internal/streams/end-of-stream.js
//===----------------------------------------------------------------------===//

export interface FinishedOptions {
  error?: boolean
  readable?: boolean
  writable?: boolean
  signal?: AbortSignal
  cleanup?: boolean
}

// What one finished() waits for, and its callback, which it drops once
// called. Its listeners hold it and take the stream as `this`, so that the
// stream does not hold itself through them: node leaves them on the stream
// after the callback, and so does this code.
class Eos {
  callback: StreamCallback
  willEmitClose: boolean
  writableFinished: boolean
  readableFinished: boolean
  closed: boolean
  constructor(
    stream: Stream,
    readonly readable: boolean,
    readonly writable: boolean,
    callback: StreamCallback,
  ) {
    this.callback = callback
    this.willEmitClose = willEmitClose(stream) && isReadableNodeStream(stream) == readable &&
      isWritableNodeStream(stream) == writable
    this.writableFinished = isWritableFinished(stream, false)
    this.readableFinished = isReadableFinished(stream, false)
    this.closed = isClosed(stream)
  }

  // The callback, once.
  call(err?: Error) {
    const callback = this.callback
    this.callback = nop
    callback(err)
  }

  finishEvent(stream: Stream) {
    this.writableFinished = true
    if (stream.destroyed) this.willEmitClose = false
    if (this.willEmitClose && (!stream.readable || this.readable)) return
    if (!this.readable || this.readableFinished) this.call()
  }

  endEvent(stream: Stream) {
    this.readableFinished = true
    if (stream.destroyed) this.willEmitClose = false
    if (this.willEmitClose && (!stream.writable || this.writable)) return
    if (!this.writable || this.writableFinished) this.call()
  }

  closeEvent(stream: Stream) {
    this.closed = true
    const errored = erroredOf(stream)
    if (errored != undefined) {
      this.call(errored)
      return
    }
    if (this.readable && !this.readableFinished && isReadableNodeStream(stream)) {
      if (!isReadableFinished(stream, false)) {
        this.call(prematureClose())
        return
      }
    }
    if (this.writable && !this.writableFinished) {
      if (!isWritableFinished(stream, false)) {
        this.call(prematureClose())
        return
      }
    }
    this.call()
  }

  closedEvent(stream: Stream) {
    this.closed = true
    const errored = erroredOf(stream)
    if (errored != undefined) this.call(errored)
    else this.call()
  }
}

// eos() of node; gives the function that removes its listeners.
function eos(stream: Stream, options: FinishedOptions | undefined,
             callback: StreamCallback): () => void {
  const readable = options?.readable ?? isReadableNodeStream(stream)
  const writable = options?.writable ?? isWritableNodeStream(stream)
  const wState = stream._writableState
  const rState = stream._readableState
  const e = new Eos(stream, readable, writable, callback)
  const onfinish = function (this: Stream) {
    e.finishEvent(this)
  }
  const onend = function (this: Stream) {
    e.endEvent(this)
  }
  const onerror = function (this: Stream, err: Error) {
    void this
    e.call(err)
  }
  const onclose = function (this: Stream) {
    e.closeEvent(this)
  }
  stream.on("end", onend, kThisListener)
  stream.on("finish", onfinish, kThisListener)
  if (options?.error != false) stream.on("error", onerror, kThisListener)
  stream.on("close", onclose, kThisListener)
  if (e.closed) {
    process.nextTick(() => e.closeEvent(stream))
  } else if ((wState != undefined && (wState.bits & kErrorEmitted) != 0) ||
             (rState != undefined && (rState.bits & kErrorEmitted) != 0)) {
    if (!e.willEmitClose) process.nextTick(() => e.closedEvent(stream))
  } else if (!readable && (!e.willEmitClose || isReadable(stream)) &&
             (e.writableFinished || !isWritable(stream)) &&
             (wState == undefined || wState.pendingcb == 0)) {
    process.nextTick(() => e.closedEvent(stream))
  } else if (!writable && (!e.willEmitClose || isWritable(stream)) &&
             (e.readableFinished || !isReadable(stream))) {
    process.nextTick(() => e.closedEvent(stream))
  }
  const cleanup = () => {
    e.callback = nop
    stream.removeListener("finish", onfinish)
    stream.removeListener("end", onend)
    stream.removeListener("error", onerror)
    stream.removeListener("close", onclose)
  }
  const optionSignal = options?.signal
  if (optionSignal != undefined && !e.closed) {
    const signal = optionSignal
    const abort = () => {
      const endCallback = e.callback
      cleanup()
      endCallback(new AbortError(undefined, errorOf(signal.reason)))
    }
    if (signal.aborted) {
      process.nextTick(abort)
    } else {
      const onAbort = (_event: Event) => abort()
      signal.addFunctionListener<Event>("abort", onAbort, { once: true })
      const originalCallback = e.callback
      e.callback = (err?: Error) => {
        signal.removeListenerOf("abort", onAbort, false)
        originalCallback(err)
      }
    }
  }
  return cleanup
}

// pipeline(), finished() and compose() of node:stream take streams and
// functions of any types, which the compiler checks: each call becomes one
// of streamPipeline(), streamFinished() or streamCompose() (see
// src/sema/sema_stream.cpp).
export function pipeline() {}
export function finished() {}
export function compose() {}

// finished() of node:stream/promises.
export function finishedPromise(stream: Stream, options?: FinishedOptions): Promise<void> {
  const autoCleanup = options?.cleanup ?? false
  return new Promise<void>((resolve, reject) => {
    let cleanup: () => void = () => {}
    cleanup = eos(stream, options, (err?: Error) => {
      if (autoCleanup) cleanup()
      if (err != undefined) reject(err)
      else resolve()
    })
  })
}

//===----------------------------------------------------------------------===//
// lib/internal/streams/add-abort-signal.js
//===----------------------------------------------------------------------===//

function addAbortSignalNoValidate(signal: AbortSignal, stream: Stream) {
  const onAbort = () => {
    stream.destroy(new AbortError(undefined, errorOf(signal.reason)))
  }
  if (signal.aborted) {
    onAbort()
  } else {
    const listener = (_event: Event) => onAbort()
    signal.addFunctionListener<Event>("abort", listener, { once: true })
    eos(stream, undefined, () => signal.removeListenerOf("abort", listener, false))
  }
}

export function addAbortSignal<S extends Stream>(signal: AbortSignal, stream: S): S {
  addAbortSignalNoValidate(signal, stream)
  return stream
}

//===----------------------------------------------------------------------===//
// The classes of the program: Readable, Writable, Duplex, Transform and
// PassThrough, which give the chunks their types
//===----------------------------------------------------------------------===//

// The events of every stream; 'data' gives a chunk of T.
export type StreamEvents<T> = {
  close: []
  data: [chunk: T]
  end: []
  error: [err: Error]
  pause: []
  readable: []
  resume: []
  drain: []
  finish: []
  prefinish: []
  pipe: [src: Stream]
  unpipe: [src: Stream, info: { hasUnpiped: boolean }]
  // Of the streams of files (node:fs).
  open: [fd: int]
  ready: []
}

// What gives the classes their event map: the compiler finds it here as it
// finds that of EventEmitter (src/sema/sema_events.cpp).
export class StreamEmitter<Events> extends Stream {}

// The sides a Duplex is being made with, which the constructors up its
// chain take instead of options of their own: a class has one constructor,
// so that the type of its options gives the literal written in `new` its
// types. A class down the chain of Duplex sets it before `super()`.
let pendingSides: DuplexSides | undefined = undefined

// The kinds of chunk types (__tz_chunkKind): a Buffer, a string, anything
// else.
const kStringChunks = 1

export interface ReadableOptions<T> {
  highWaterMark?: double
  encoding?: string
  objectMode?: boolean
  emitClose?: boolean
  autoDestroy?: boolean
  signal?: AbortSignal
  read?: (this: Readable<T>, size: int) => void
  destroy?: (this: Readable<T>, error: Error | undefined, callback: StreamCallback) => void
  construct?: (this: Readable<T>, callback: StreamCallback) => void
}

// A stream of strings without an encoding decodes UTF-8, so that its chunks
// are strings; one of Buffers has none.
function chunkEncoding(kind: int, objectMode: boolean, encoding: string | undefined) {
  if (objectMode || kind != kStringChunks) return encoding
  return encoding ?? "utf8"
}

export class Readable<T = Buffer> extends StreamEmitter<StreamEvents<T>> {
  #readOption: ((this: Readable<T>, size: int) => void) | undefined
  #destroyOption:
    | ((this: Readable<T>, error: Error | undefined, callback: StreamCallback) => void)
    | undefined
  #constructOption: ((this: Readable<T>, callback: StreamCallback) => void) | undefined

  constructor(options?: ReadableOptions<T>) {
    super()
    this.#readOption = options?.read
    this.#destroyOption = options?.destroy
    this.#constructOption = options?.construct
    // A Duplex makes both sides itself.
    if (pendingSides != undefined) return
    this._initReadable(
      {
        highWaterMark: options?.highWaterMark,
        objectMode: options?.objectMode,
        encoding: chunkEncoding(__tz_chunkKind<T>(), options?.objectMode ?? false,
                                options?.encoding),
        emitClose: options?.emitClose,
        autoDestroy: options?.autoDestroy,
      },
      false,
    )
    this._hasConstruct = options?.construct != undefined || __tz_overrides(this, "_construct")
    this._initEnd(options?.signal)
  }

  _read(size: int): void {
    const read = this.#readOption
    if (read == undefined) throw methodNotImplemented("_read()")
    read.call(this, size)
  }

  _destroy(error: Error | undefined, callback: StreamCallback): void {
    const destroy = this.#destroyOption
    if (destroy == undefined) callback(error)
    else destroy.call(this, error, callback)
  }

  _construct(callback: StreamCallback): void {
    const construct = this.#constructOption
    if (construct == undefined) callback()
    else construct.call(this, callback)
  }

  // A chunk goes to the listeners of 'data' as what they take: a T in
  // object mode; otherwise a string once the stream has an encoding, or a
  // Buffer.
  _emitData(chunk: unknown): void {
    const state = this._readableState
    if (state == undefined || (state.bits & kObjectMode) != 0)
      this.emitWithUnknown("data", chunk, __tz_chunk<T>(chunk))
    else if (typeof chunk == "string") this.emitWithUnknown("data", chunk, chunk)
    else this.emitWithUnknown("data", chunk, __tz_chunk<Buffer>(chunk))
  }

  // read(size) of node, which gives undefined where node gives null.
  read(size?: int): T | undefined {
    const chunk = this._readChunk(size)
    return chunk == undefined ? undefined : __tz_chunk<T>(chunk)
  }

  [Symbol.asyncIterator](): AsyncGenerator<T> {
    return createAsyncIterator<T>(this, true)
  }

  iterator(options?: { destroyOnReturn?: boolean }): AsyncGenerator<T> {
    return createAsyncIterator<T>(this, options?.destroyOnReturn ?? true)
  }
}

// What one `for await` over a stream waits on: node's createAsyncIterator.
class IteratorWait {
  callback: () => void = noWait
  // The end of the stream: `ended` without an error, or the error.
  ended = false
  error: Error | undefined = undefined

  wake() {
    const callback = this.callback
    this.callback = noWait
    callback()
  }
}

function noWait() {}

async function* createAsyncIterator<T>(stream: Stream, destroyOnReturn: boolean):
    AsyncGenerator<T> {
  const wait = new IteratorWait()
  const next = function (this: Stream) {
    void this
    wait.wake()
  }
  stream.on("readable", next, kThisListener)
  const cleanup = eos(stream, { writable: false }, (err?: Error) => {
    if (err != undefined) wait.error = aggregateTwoErrors(wait.error, err)
    else wait.ended = true
    wait.wake()
  })
  let failed = false
  try {
    while (true) {
      const chunk = stream.destroyed ? undefined : stream._readChunk()
      if (chunk != undefined) {
        yield __tz_chunk<T>(chunk)
      } else {
        const error = wait.error
        if (error != undefined) throw error
        if (wait.ended) return
        await new Promise<void>((resolve) => {
          wait.callback = resolve
        })
      }
    }
  } catch (err) {
    failed = true
    const error = aggregateTwoErrors(wait.error, err) ?? err
    wait.error = error
    throw error
  } finally {
    // The listeners hold what the iterator waits on, not the iterator: those
    // left on a stream it destroys, as node leaves them, make no cycle.
    const r = stream._readableState
    if ((failed || destroyOnReturn) &&
        ((!failed && !wait.ended) || (r != undefined && (r.bits & kAutoDestroy) != 0))) {
      destroyer(stream, undefined)
    } else {
      stream.removeListener("readable", next)
      cleanup()
    }
  }
}

export interface WriteChunk<T> {
  chunk: T
  encoding: string
}

export interface WritableOptions<T> {
  highWaterMark?: double
  decodeStrings?: boolean
  defaultEncoding?: string
  objectMode?: boolean
  emitClose?: boolean
  autoDestroy?: boolean
  signal?: AbortSignal
  write?: (this: Writable<T>, chunk: T, encoding: string, callback: StreamCallback) => void
  writev?: (this: Writable<T>, chunks: WriteChunk<T>[], callback: StreamCallback) => void
  final?: (this: Writable<T>, callback: StreamCallback) => void
  destroy?: (this: Writable<T>, error: Error | undefined, callback: StreamCallback) => void
  construct?: (this: Writable<T>, callback: StreamCallback) => void
}

// The chunks of a writev() as their type.
function typedChunks<T>(chunks: BufferedWrite[]): WriteChunk<T>[] {
  const out: WriteChunk<T>[] = []
  for (const c of chunks) out.push({ chunk: __tz_chunk<T>(c.chunk), encoding: c.encoding })
  return out
}

// A stream of strings keeps the strings written; one of Buffers gets them as
// Buffers.
function decodesStrings(kind: int, objectMode: boolean) {
  return objectMode || kind != kStringChunks
}

export class Writable<T = Buffer> extends StreamEmitter<StreamEvents<T>> {
  // A Duplex works where a Writable is expected (see the top of this file):
  // only the methods that replace those of Stream use these fields, and in a
  // Duplex those are the Duplex's own.
  #writeOption:
    | ((this: Writable<T>, chunk: T, encoding: string, callback: StreamCallback) => void)
    | undefined
  #writevOption:
    | ((this: Writable<T>, chunks: WriteChunk<T>[], callback: StreamCallback) => void)
    | undefined
  #finalOption: ((this: Writable<T>, callback: StreamCallback) => void) | undefined
  #destroyOption:
    | ((this: Writable<T>, error: Error | undefined, callback: StreamCallback) => void)
    | undefined
  #constructOption: ((this: Writable<T>, callback: StreamCallback) => void) | undefined

  constructor(options?: WritableOptions<T>) {
    super()
    this.#writeOption = options?.write
    this.#writevOption = options?.writev
    this.#finalOption = options?.final
    this.#destroyOption = options?.destroy
    this.#constructOption = options?.construct
    this._initWritable(
      {
        highWaterMark: options?.highWaterMark,
        objectMode: options?.objectMode,
        decodeStrings: decodesStrings(__tz_chunkKind<T>(), options?.objectMode ?? false),
        defaultEncoding: options?.defaultEncoding,
        emitClose: options?.emitClose,
        autoDestroy: options?.autoDestroy,
      },
      false,
    )
    this._hasWritev = options?.writev != undefined || __tz_overrides(this, "_writev")
    this._hasFinal = options?.final != undefined || __tz_overrides(this, "_final")
    this._hasConstruct = options?.construct != undefined || __tz_overrides(this, "_construct")
    this._initEnd(options?.signal)
  }

  _writeChunk(chunk: unknown, encoding: string, callback: StreamCallback): void {
    this._write(__tz_chunk<T>(chunk), encoding, callback)
  }

  _writevChunks(chunks: BufferedWrite[], callback: StreamCallback): void {
    this._writev(typedChunks<T>(chunks), callback)
  }

  _write(chunk: T, encoding: string, callback: StreamCallback): void {
    const write = this.#writeOption
    if (write != undefined) write.call(this, chunk, encoding, callback)
    else if (this._hasWritev) this._writev([{ chunk, encoding }], callback)
    else throw methodNotImplemented("_write()")
  }

  _writev(chunks: WriteChunk<T>[], callback: StreamCallback): void {
    const writev = this.#writevOption
    if (writev == undefined) throw methodNotImplemented("_writev()")
    writev.call(this, chunks, callback)
  }

  _final(callback: StreamCallback): void {
    const final = this.#finalOption
    if (final == undefined) callback()
    else final.call(this, callback)
  }

  _destroy(error: Error | undefined, callback: StreamCallback): void {
    const destroy = this.#destroyOption
    if (destroy == undefined) callback(error)
    else destroy.call(this, error, callback)
  }

  _construct(callback: StreamCallback): void {
    const construct = this.#constructOption
    if (construct == undefined) callback()
    else construct.call(this, callback)
  }
}

export interface DuplexOptions<W, R> {
  allowHalfOpen?: boolean
  readable?: boolean
  writable?: boolean
  highWaterMark?: double
  readableHighWaterMark?: double
  writableHighWaterMark?: double
  objectMode?: boolean
  readableObjectMode?: boolean
  writableObjectMode?: boolean
  encoding?: string
  decodeStrings?: boolean
  defaultEncoding?: string
  emitClose?: boolean
  autoDestroy?: boolean
  signal?: AbortSignal
  read?: (this: Duplex<W, R>, size: int) => void
  write?: (this: Duplex<W, R>, chunk: W, encoding: string, callback: StreamCallback) => void
  writev?: (this: Duplex<W, R>, chunks: WriteChunk<W>[], callback: StreamCallback) => void
  final?: (this: Duplex<W, R>, callback: StreamCallback) => void
  destroy?: (this: Duplex<W, R>, error: Error | undefined, callback: StreamCallback) => void
  construct?: (this: Duplex<W, R>, callback: StreamCallback) => void
}

// What a Duplex and the classes down its chain give the constructor of
// Duplex: the options for the sides, without the functions.
class DuplexSides {
  allowHalfOpen: boolean | undefined = undefined
  readable: boolean | undefined = undefined
  writable: boolean | undefined = undefined
  highWaterMark: double | undefined = undefined
  readableHighWaterMark: double | undefined = undefined
  writableHighWaterMark: double | undefined = undefined
  objectMode: boolean | undefined = undefined
  readableObjectMode: boolean | undefined = undefined
  writableObjectMode: boolean | undefined = undefined
  encoding: string | undefined = undefined
  decodeStrings: boolean | undefined = undefined
  defaultEncoding: string | undefined = undefined
  emitClose: boolean | undefined = undefined
  autoDestroy: boolean | undefined = undefined
  signal: AbortSignal | undefined = undefined
  hasWritev = false
  hasFinal = false
  hasConstruct = false
  // A Transform, whose constructor ends the making itself (see _initEnd).
  transform = false
}

// A Duplex<W, R> takes chunks of W and gives chunks of R.
export class Duplex<W = Buffer, R = W> extends Readable<R> {
  @__tz_hidden _duplexRead: ((this: Duplex<W, R>, size: int) => void) | undefined
  @__tz_hidden _duplexWrite:
    | ((this: Duplex<W, R>, chunk: W, encoding: string, callback: StreamCallback) => void)
    | undefined
  @__tz_hidden _duplexWritev:
    | ((this: Duplex<W, R>, chunks: WriteChunk<W>[], callback: StreamCallback) => void)
    | undefined
  @__tz_hidden _duplexFinal: ((this: Duplex<W, R>, callback: StreamCallback) => void) | undefined
  @__tz_hidden _duplexDestroy:
    | ((this: Duplex<W, R>, error: Error | undefined, callback: StreamCallback) => void)
    | undefined
  @__tz_hidden _duplexConstruct:
    | ((this: Duplex<W, R>, callback: StreamCallback) => void)
    | undefined

  constructor(options?: DuplexOptions<W, R>) {
    // The sides of a class down the chain, or those of the options.
    const given = pendingSides
    if (given == undefined) pendingSides = new DuplexSides()
    super()
    pendingSides = undefined
    this._duplexRead = options?.read
    this._duplexWrite = options?.write
    this._duplexWritev = options?.writev
    this._duplexFinal = options?.final
    this._duplexDestroy = options?.destroy
    this._duplexConstruct = options?.construct
    if (given != undefined) {
      this._initDuplex(given)
      return
    }
    const sides = new DuplexSides()
    sides.allowHalfOpen = options?.allowHalfOpen
    sides.readable = options?.readable
    sides.writable = options?.writable
    sides.highWaterMark = options?.highWaterMark
    sides.readableHighWaterMark = options?.readableHighWaterMark
    sides.writableHighWaterMark = options?.writableHighWaterMark
    sides.objectMode = options?.objectMode
    sides.readableObjectMode = options?.readableObjectMode
    sides.writableObjectMode = options?.writableObjectMode
    sides.encoding = options?.encoding
    sides.decodeStrings = options?.decodeStrings
    sides.defaultEncoding = options?.defaultEncoding
    sides.emitClose = options?.emitClose
    sides.autoDestroy = options?.autoDestroy
    sides.signal = options?.signal
    sides.hasWritev = options?.writev != undefined
    sides.hasFinal = options?.final != undefined
    sides.hasConstruct = options?.construct != undefined
    this._initDuplex(sides)
  }

  // node's Duplex constructor.
  _initDuplex(sides: DuplexSides) {
    const readableObjectMode = sides.objectMode == true || sides.readableObjectMode == true
    const writableObjectMode = sides.objectMode == true || sides.writableObjectMode == true
    this._initReadable(
      {
        highWaterMark: sides.highWaterMark,
        sideHighWaterMark: sides.readableHighWaterMark,
        objectMode: sides.objectMode,
        sideObjectMode: sides.readableObjectMode,
        encoding: chunkEncoding(__tz_chunkKind<R>(), readableObjectMode, sides.encoding),
        defaultEncoding: sides.defaultEncoding,
        emitClose: sides.emitClose,
        autoDestroy: sides.autoDestroy,
      },
      true,
    )
    this._initWritable(
      {
        highWaterMark: sides.highWaterMark,
        sideHighWaterMark: sides.writableHighWaterMark,
        objectMode: sides.objectMode,
        sideObjectMode: sides.writableObjectMode,
        decodeStrings: decodesStrings(__tz_chunkKind<W>(), writableObjectMode),
        defaultEncoding: sides.defaultEncoding,
        emitClose: sides.emitClose,
        autoDestroy: sides.autoDestroy,
      },
      true,
    )
    this.allowHalfOpen = sides.allowHalfOpen != false
    const r = readableStateOf(this)
    const w = writableStateOf(this)
    if (sides.readable == false) {
      r.readable = false
      r.bits |= kEnded | kEndEmitted
    }
    if (sides.writable == false) {
      w.writable = false
      w.bits |= kEnding | kWEnded | kFinished
    }
    sync(this)
    // Whether the hooks are there: the program's class replaces those of
    // Duplex. A Transform replaces them itself, and asks again.
    if (sides.transform) return
    this._hasWritev = sides.hasWritev || __tz_overrides(this, "_writev")
    this._hasFinal = sides.hasFinal || __tz_overrides(this, "_final")
    this._hasConstruct = sides.hasConstruct || __tz_overrides(this, "_construct")
    this._initEnd(sides.signal)
  }

  _read(size: int): void {
    const read = this._duplexRead
    if (read == undefined) throw methodNotImplemented("_read()")
    read.call(this, size)
  }

  _writeChunk(chunk: unknown, encoding: string, callback: StreamCallback): void {
    this._write(__tz_chunk<W>(chunk), encoding, callback)
  }

  _writevChunks(chunks: BufferedWrite[], callback: StreamCallback): void {
    this._writev(typedChunks<W>(chunks), callback)
  }

  _write(chunk: W, encoding: string, callback: StreamCallback): void {
    const write = this._duplexWrite
    if (write != undefined) write.call(this, chunk, encoding, callback)
    else if (this._hasWritev) this._writev([{ chunk, encoding }], callback)
    else throw methodNotImplemented("_write()")
  }

  _writev(chunks: WriteChunk<W>[], callback: StreamCallback): void {
    const writev = this._duplexWritev
    if (writev == undefined) throw methodNotImplemented("_writev()")
    writev.call(this, chunks, callback)
  }

  _final(callback: StreamCallback): void {
    const final = this._duplexFinal
    if (final == undefined) callback()
    else final.call(this, callback)
  }

  _destroy(error: Error | undefined, callback: StreamCallback): void {
    const destroy = this._duplexDestroy
    if (destroy == undefined) callback(error)
    else destroy.call(this, error, callback)
  }

  _construct(callback: StreamCallback): void {
    const construct = this._duplexConstruct
    if (construct == undefined) callback()
    else construct.call(this, callback)
  }
}

export interface TransformOptions<W, R> {
  allowHalfOpen?: boolean
  readable?: boolean
  writable?: boolean
  highWaterMark?: double
  readableHighWaterMark?: double
  writableHighWaterMark?: double
  objectMode?: boolean
  readableObjectMode?: boolean
  writableObjectMode?: boolean
  encoding?: string
  decodeStrings?: boolean
  defaultEncoding?: string
  emitClose?: boolean
  autoDestroy?: boolean
  signal?: AbortSignal
  read?: (this: Transform<W, R>, size: int) => void
  write?: (this: Transform<W, R>, chunk: W, encoding: string, callback: StreamCallback) => void
  writev?: (this: Transform<W, R>, chunks: WriteChunk<W>[], callback: StreamCallback) => void
  final?: (this: Transform<W, R>, callback: StreamCallback) => void
  destroy?: (this: Transform<W, R>, error: Error | undefined, callback: StreamCallback) => void
  construct?: (this: Transform<W, R>, callback: StreamCallback) => void
  transform?: (this: Transform<W, R>, chunk: W, encoding: string,
               callback: TransformCallback) => void
  flush?: (this: Transform<W, R>, callback: TransformCallback) => void
}

// The sides of a Transform: a readable side that buffers nothing makes the
// writable one buffer nothing either, unless it is told otherwise.
function transformSides<W, R>(options: TransformOptions<W, R> | undefined): DuplexSides {
  const sides = new DuplexSides()
  const hwm = options?.highWaterMark ?? options?.readableHighWaterMark
  const zero = hwm == 0
  sides.allowHalfOpen = options?.allowHalfOpen
  sides.readable = options?.readable
  sides.writable = options?.writable
  sides.highWaterMark = zero ? undefined : options?.highWaterMark
  sides.readableHighWaterMark = zero ? 0 : options?.readableHighWaterMark
  sides.writableHighWaterMark = zero
    ? options?.writableHighWaterMark ?? 0
    : options?.writableHighWaterMark
  sides.objectMode = options?.objectMode
  sides.readableObjectMode = options?.readableObjectMode
  sides.writableObjectMode = options?.writableObjectMode
  sides.encoding = options?.encoding
  sides.decodeStrings = options?.decodeStrings
  sides.defaultEncoding = options?.defaultEncoding
  sides.emitClose = options?.emitClose
  sides.autoDestroy = options?.autoDestroy
  sides.signal = options?.signal
  sides.hasWritev = options?.writev != undefined
  // Transform.prototype._final of node.
  sides.hasFinal = true
  sides.hasConstruct = options?.construct != undefined
  sides.transform = true
  return sides
}

// lib/internal/streams/transform.js: a Duplex whose readable side gives
// what _transform() makes of what is written.
export class Transform<W = Buffer, R = W> extends Duplex<W, R> {
  @__tz_hidden _transformRead: ((this: Transform<W, R>, size: int) => void) | undefined
  @__tz_hidden _transformWrite:
    | ((this: Transform<W, R>, chunk: W, encoding: string, callback: StreamCallback) => void)
    | undefined
  @__tz_hidden _transformWritev:
    | ((this: Transform<W, R>, chunks: WriteChunk<W>[], callback: StreamCallback) => void)
    | undefined
  @__tz_hidden _transformFinal:
    | ((this: Transform<W, R>, callback: StreamCallback) => void)
    | undefined
  @__tz_hidden _transformDestroy:
    | ((this: Transform<W, R>, error: Error | undefined, callback: StreamCallback) => void)
    | undefined
  @__tz_hidden _transformConstruct:
    | ((this: Transform<W, R>, callback: StreamCallback) => void)
    | undefined
  @__tz_hidden _transformOption:
    | ((this: Transform<W, R>, chunk: W, encoding: string, callback: TransformCallback) => void)
    | undefined
  @__tz_hidden _flushOption: ((this: Transform<W, R>, callback: TransformCallback) => void) |
    undefined

  constructor(options?: TransformOptions<W, R>) {
    pendingSides = transformSides(options)
    super()
    this._transformRead = options?.read
    this._transformWrite = options?.write
    this._transformWritev = options?.writev
    this._transformFinal = options?.final
    this._transformDestroy = options?.destroy
    this._transformConstruct = options?.construct
    this._transformOption = options?.transform
    this._flushOption = options?.flush
    this._isTransform = true
    readableStateOf(this).bits &= ~kSync
    this._hasWritev = options?.writev != undefined || __tz_overrides(this, "_writev")
    this._hasFinal = true
    this._hasConstruct = options?.construct != undefined || __tz_overrides(this, "_construct")
    this._hasFlush = options?.flush != undefined || __tz_overrides(this, "_flush")
    // node's prefinish listener, which takes the stream as `this`.
    this.on("prefinish", transformPrefinish, kThisListener)
    this._initEnd(options?.signal)
  }

  _read(size: int): void {
    const read = this._transformRead
    if (read != undefined) {
      read.call(this, size)
      return
    }
    const callback = this._transformCallback
    if (callback != undefined) {
      this._transformCallback = undefined
      callback()
    }
  }

  _write(chunk: W, encoding: string, callback: StreamCallback): void {
    const write = this._transformWrite
    if (write != undefined) {
      write.call(this, chunk, encoding, callback)
      return
    }
    transformWrite<W, R>(this, chunk, encoding, callback)
  }

  _writev(chunks: WriteChunk<W>[], callback: StreamCallback): void {
    const writev = this._transformWritev
    if (writev == undefined) throw methodNotImplemented("_writev()")
    writev.call(this, chunks, callback)
  }

  _final(callback: StreamCallback): void {
    const final = this._transformFinal
    if (final != undefined) {
      final.call(this, callback)
      return
    }
    transformFinal(this, callback)
  }

  _destroy(error: Error | undefined, callback: StreamCallback): void {
    const destroy = this._transformDestroy
    if (destroy == undefined) callback(error)
    else destroy.call(this, error, callback)
  }

  _construct(callback: StreamCallback): void {
    const construct = this._transformConstruct
    if (construct == undefined) callback()
    else construct.call(this, callback)
  }

  _transform(chunk: W, encoding: string, callback: TransformCallback): void {
    const transform = this._transformOption
    if (transform == undefined) throw methodNotImplemented("_transform()")
    transform.call(this, chunk, encoding, callback)
  }

  _flush(callback: TransformCallback): void {
    const flush = this._flushOption
    if (flush == undefined) callback()
    else flush.call(this, callback)
  }

  _ownTransformFinal(): boolean {
    return this._transformFinal == undefined && !__tz_overrides(this, "_final")
  }
}

// node's final() of Transform: flushes, then ends the readable side. With
// no callback it is the prefinish listener's, whose error destroys.
function transformFinal(stream: Stream, cb: StreamCallback | undefined) {
  if (stream._hasFlush && !stream.destroyed) {
    stream._flush((er?: Error, data?: unknown) => {
      if (er != undefined) {
        if (cb != undefined) cb(er)
        else stream.destroy(er)
        return
      }
      if (data != undefined) stream._pushChunk(data)
      stream._pushChunk(undefined)
      if (cb != undefined) cb()
    })
  } else {
    stream._pushChunk(undefined)
    if (cb != undefined) cb()
  }
}

// The prefinish listener of a Transform: final() of node, unless _final is
// Transform's own, which Writable calls itself.
function transformPrefinish(this: Stream) {
  if (!this._ownTransformFinal()) transformFinal(this, undefined)
}

function transformWrite<W, R>(stream: Transform<W, R>, chunk: W, encoding: string,
                              callback: StreamCallback) {
  const rState = readableStateOf(stream)
  const wState = writableStateOf(stream)
  const length = stream.readableLength
  stream._transform(chunk, encoding, (err?: Error, val?: unknown) => {
    if (err != undefined) {
      callback(err)
      return
    }
    if (val != undefined) stream._pushChunk(val)
    if ((rState.bits & kEnded) != 0) {
      process.nextTick(() => callback())
    } else if ((wState.bits & kWEnded) != 0 || // Backwards compat.
               length == stream.readableLength || // Backwards compat.
               stream.readableLength < stream.readableHighWaterMark) {
      callback()
    } else {
      stream._transformCallback = callback
    }
  })
}

// lib/internal/streams/passthrough.js: a Transform that gives what it gets
// (as strings, with an encoding of its readable side).
export class PassThrough<W = Buffer, R = W> extends Transform<W, R> {
  constructor(options?: TransformOptions<W, R>) {
    super(options)
  }

  _transform(chunk: W, encoding: string, callback: TransformCallback): void {
    void encoding
    callback(undefined, chunk)
  }
}

//===----------------------------------------------------------------------===//
// lib/internal/streams/from.js: Readable.from()
//===----------------------------------------------------------------------===//

// The options of Readable.from(), which those of a Readable are.
export interface ReadableFromOptions {
  objectMode?: boolean
  highWaterMark?: double
  encoding?: string
  emitClose?: boolean
  autoDestroy?: boolean
  signal?: AbortSignal
}

function fromOptions<U>(options: ReadableFromOptions | undefined, hwm: double | undefined):
    ReadableOptions<U> {
  return {
    objectMode: options?.objectMode ?? true,
    highWaterMark: options?.highWaterMark ?? hwm,
    encoding: options?.encoding,
    emitClose: options?.emitClose,
    autoDestroy: options?.autoDestroy,
    signal: options?.signal,
  }
}

// Readable.from(text) and Readable.from(buffer): one chunk, the value
// itself (the compiler calls this).
export function streamFromValue<U>(value: U, options?: ReadableFromOptions): Readable<U> {
  return new FromValue<U>(value, fromOptions<U>(options, undefined))
}

class FromValue<U> extends Readable<U> {
  #value: U | undefined
  constructor(value: U, options: ReadableOptions<U>) {
    super(options)
    this.#value = value
  }

  _read(size: int): void {
    void size
    const value = this.#value
    this.#value = undefined
    if (value != undefined) this._pushChunk(value)
    this._pushChunk(undefined)
  }
}

// Readable.from(iterable) of an iterable that is not async.
export function streamFromIterator<U>(iterator: Iterator<U>, options?: ReadableFromOptions):
    Readable<U> {
  return new FromIterator<U>(iterator, fromOptions<U>(options, 1))
}

class FromIterator<U> extends Readable<U> {
  #iterator: Iterator<U>
  #reading = false
  constructor(iterator: Iterator<U>, options: ReadableOptions<U>) {
    super(options)
    this.#iterator = iterator
  }

  _read(size: int): void {
    void size
    if (!this.#reading) {
      this.#reading = true
      this.#next()
    }
  }

  #next() {
    for (;;) {
      try {
        const result = this.#iterator.next()
        if (result.done) {
          this._pushChunk(undefined)
          return
        }
        const value: unknown = result.value
        if (value == undefined) {
          this.#reading = false
          throw nullValues()
        }
        if (this._pushChunk(value)) continue
        this.#reading = false
      } catch (err) {
        this.destroy(err)
      }
      break
    }
  }
}

// Readable.from(iterable) of an async iterable.
export function streamFromAsyncIterator<U>(iterator: AsyncIterator<U>,
                                           options?: ReadableFromOptions): Readable<U> {
  return new FromAsyncIterator<U>(iterator, fromOptions<U>(options, 1))
}

// node takes the values one at a time, with `await iterator.next()` as
// _read() asks; here a `for await` over the iterator waits for _read()
// where node would stop asking, and leaving it (on destroy) closes the
// iterator, as node's return() does.
class FromAsyncIterator<U> extends Readable<U> {
  #iterator: AsyncIterator<U>
  #started = false
  #wake: (() => void) | undefined = undefined
  constructor(iterator: AsyncIterator<U>, options: ReadableOptions<U>) {
    super(options)
    this.#iterator = iterator
  }

  _read(size: int): void {
    void size
    if (!this.#started) {
      this.#started = true
      void this.#run()
      return
    }
    this.#resume()
  }

  #resume() {
    const wake = this.#wake
    this.#wake = undefined
    if (wake != undefined) wake()
  }

  async #run() {
    try {
      for await (const value of this.#iterator) {
        const chunk: unknown = value
        if (chunk == undefined) throw nullValues()
        if (!this._pushChunk(chunk))
          await new Promise<void>((resolve) => {
            this.#wake = resolve
          })
        if (this.destroyed) return
      }
      this._pushChunk(undefined)
    } catch (err) {
      this.destroy(err)
    }
  }

  _destroy(error: Error | undefined, callback: StreamCallback): void {
    // The loop ends, which closes the iterator; then the callback, on the
    // next tick in case it throws.
    this.#resume()
    process.nextTick(() => callback(error))
  }
}

//===----------------------------------------------------------------------===//
// lib/internal/streams/pipeline.js
//===----------------------------------------------------------------------===//

// A function among the streams of pipeline(), which the compiler gives in a
// Stage: the source of the chunks (the first), a transform of the chunks
// before it, which gives those after it, or the sink (the last), whose
// Promise ends the pipeline.
const kSourceStage = 0
const kTransformStage = 1
const kSinkStage = 2

export class PipelineStage {
  constructor(
    readonly kind: int,
    readonly source: ((signal: AbortSignal) => AsyncGenerator<unknown>) | undefined,
    readonly transform:
      | ((source: AsyncGenerator<unknown>, signal: AbortSignal) => AsyncGenerator<unknown>)
      | undefined,
    readonly sink: ((source: AsyncGenerator<unknown>, signal: AbortSignal) => Promise<unknown>) |
      undefined,
  ) {}
}

async function* chunksAs<A>(source: AsyncGenerator<unknown>): AsyncGenerator<A> {
  for await (const chunk of source) yield __tz_chunk<A>(chunk)
}

async function* chunksOf<B>(source: AsyncGenerator<B>): AsyncGenerator<unknown> {
  for await (const chunk of source) yield chunk
}

async function* iteratorChunks<B>(source: AsyncIterator<B>): AsyncGenerator<unknown> {
  for await (const chunk of source) yield chunk
}

function* syncChunks<B>(source: Iterator<B>): Generator<unknown> {
  while (true) {
    const result = source.next()
    if (result.done) return
    yield result.value
  }
}

async function* asyncChunks(source: Generator<unknown>): AsyncGenerator<unknown> {
  for (const chunk of source) yield chunk
}

// The stages the compiler makes of the functions and iterables given to
// pipeline().
export function streamSourceStage<B>(fn: (options: { signal: AbortSignal }) => AsyncGenerator<B>):
    PipelineStage {
  return new PipelineStage(kSourceStage, (signal) => chunksOf<B>(fn({ signal })), undefined,
                           undefined)
}

export function streamIterableStage<B>(iterator: Iterator<B>): PipelineStage {
  const chunks = syncChunks<B>(iterator)
  return new PipelineStage(kSourceStage, (signal) => {
    void signal
    return asyncChunks(chunks)
  }, undefined, undefined)
}

export function streamAsyncIterableStage<B>(iterator: AsyncIterator<B>): PipelineStage {
  return new PipelineStage(kSourceStage, (signal) => {
    void signal
    return iteratorChunks<B>(iterator)
  }, undefined, undefined)
}

export function streamTransformStage<A, B>(
  fn: (source: AsyncGenerator<A>, options: { signal: AbortSignal }) => AsyncGenerator<B>,
): PipelineStage {
  return new PipelineStage(
    kTransformStage,
    undefined,
    (source, signal) => chunksOf<B>(fn(chunksAs<A>(source), { signal })),
    undefined,
  )
}

export function streamSinkStage<A, V>(
  fn: (source: AsyncGenerator<A>, options: { signal: AbortSignal }) => Promise<V>,
): PipelineStage {
  return new PipelineStage(kSinkStage, undefined, undefined, async (source, signal) => {
    const value: unknown = await fn(chunksAs<A>(source), { signal })
    return value
  })
}

export function streamVoidSinkStage<A>(
  fn: (source: AsyncGenerator<A>, options: { signal: AbortSignal }) => Promise<void>,
): PipelineStage {
  return new PipelineStage(kSinkStage, undefined, undefined, async (source, signal) => {
    await fn(chunksAs<A>(source), { signal })
    const value: unknown = undefined
    return value
  })
}

// What one pipeline() keeps: node's closures share these.
class PipelineState {
  error: Error | undefined = undefined
  value: unknown = undefined
  destroys: ((err: Error | undefined) => void)[] = []
  lastStreamCleanup: (() => void)[] = []
  finishCount: int = 0
  readonly ac = new AbortController()
  constructor(public callback: ((err: Error | undefined, value: unknown) => void) | undefined) {}
}

// node's destroyer() of pipeline.js; gives the cleanup of its eos(). What
// the listeners it leaves on the stream share holds no stream.
class PipelineDestroyer {
  finished = false
}

function pipelineDestroyer(stream: Stream, reading: boolean, writing: boolean,
                           ps: PipelineState): () => void {
  const d = new PipelineDestroyer()
  stream.on("close", function (this: Stream) {
    void this
    d.finished = true
  }, kThisListener)
  const cleanup = eos(stream, { readable: reading, writable: writing }, (err?: Error) => {
    d.finished = err == undefined
  })
  ps.destroys.push((err: Error | undefined) => {
    if (d.finished) return
    d.finished = true
    destroyer(stream, err ?? streamDestroyed("pipe"))
  })
  return cleanup
}

function finishImpl(ps: PipelineState, err: Error | undefined, final: boolean) {
  if (err != undefined && (ps.error == undefined || codeOf(ps.error) == "ERR_STREAM_PREMATURE_CLOSE"))
    ps.error = err
  if (ps.error == undefined && !final) return
  while (ps.destroys.length > 0) {
    const destroy = ps.destroys.shift()
    if (destroy != undefined) destroy(ps.error)
  }
  ps.ac.abort()
  if (final) {
    if (ps.error == undefined) for (const fn of ps.lastStreamCleanup) fn()
    ps.lastStreamCleanup = []
    const callback = ps.callback
    ps.callback = undefined
    const error = ps.error
    const value = ps.value
    if (callback != undefined) {
      const done = callback
      process.nextTick(() => done(error, value))
    }
  }
}

function pipelineFinish(ps: PipelineState, err?: Error) {
  finishImpl(ps, err, --ps.finishCount == 0)
}

// The chunks of a readable stream as an async iterator: makeAsyncIterable.
function readableChunks(stream: Stream): AsyncGenerator<unknown> {
  return createAsyncIterator<unknown>(stream, true)
}

// node's pumpToNode: writes the chunks of an iterator to a stream.
async function pumpToNode(iterable: AsyncGenerator<unknown>, writable: Stream, ps: PipelineState,
                          end: boolean) {
  let error: Error | undefined = undefined
  let onresolve: (() => void) | undefined = undefined
  const resume = (err?: Error) => {
    if (err != undefined) error = err
    const callback = onresolve
    if (callback != undefined) {
      onresolve = undefined
      callback()
    }
  }
  const wait = () =>
    new Promise<void>((resolve, reject) => {
      const failed = error
      if (failed != undefined) {
        reject(failed)
      } else {
        onresolve = () => {
          const failedNow = error
          if (failedNow != undefined) reject(failedNow)
          else resolve()
        }
      }
    })
  const ondrain = () => resume()
  writable.on("drain", ondrain)
  const cleanup = eos(writable, { readable: false }, resume)
  try {
    if (writable.writableNeedDrain) await wait()
    for await (const chunk of iterable) {
      if (writable._writeAny(chunk, undefined, nop) != true) await wait()
    }
    if (end) {
      writable._endWith(false, undefined, undefined, undefined)
      await wait()
    }
    pipelineFinish(ps)
  } catch (err) {
    pipelineFinish(ps, error != err ? aggregateTwoErrors(error, err) : err)
  } finally {
    cleanup()
    writable.off("drain", ondrain)
    onresolve = undefined
  }
}

// pipe() of pipeline.js: src.pipe(dst) with what ends both.
function pipelinePipe(src: Stream, dst: Stream, ps: PipelineState, end: boolean): () => void {
  const pp = new PipelineDestroyer()
  dst.on("close", function (this: Stream) {
    void this
    if (!pp.finished) finishImpl(ps, prematureClose(), false)
  }, kThisListener)
  // If end is true we already will have a listener to end dst.
  src.pipe(dst, { end: false })
  if (end) {
    // Compat. Before node v10.12.0 stdio used to throw an error so pipe()
    // did/does not end() stdio destinations. Now they allow it but "secretly"
    // don't close the underlying fd.
    const endFn = () => {
      pp.finished = true
      dst._endWith(false, undefined, undefined, undefined)
    }
    // End the destination if the source has already ended.
    if (isReadableFinished(src, true)) process.nextTick(endFn)
    else src.once("end", endFn)
  } else {
    pipelineFinish(ps)
  }
  eos(src, { readable: true, writable: false }, (err?: Error) => {
    const rState = src._readableState
    if (err != undefined && codeOf(err) == "ERR_STREAM_PREMATURE_CLOSE" && rState != undefined &&
        (rState.bits & kEnded) != 0 && rState.errored == undefined &&
        (rState.bits & kErrorEmitted) == 0) {
      // Some readable streams will emit 'close' before 'end'. However, since
      // this is on the readable side 'end' should still be emitted if the
      // stream has been ended and no error emitted. This should be allowed in
      // favor of backwards compatibility. Since the stream is piped to a
      // destination this should not result in any observable difference. We
      // don't need to check if this is a writable premature close since eos
      // will only fail with premature close on the reading side for duplex
      // streams.
      const finishFn = () => pipelineFinish(ps)
      src.once("end", finishFn)
      src.once("error", (e: Error) => pipelineFinish(ps, e))
    } else {
      pipelineFinish(ps, err)
    }
  })
  return eos(dst, { readable: false, writable: true }, (err?: Error) => pipelineFinish(ps, err))
}

// node's pipelineImpl, of streams and the stages of functions; gives the
// last stream.
function pipelineImpl(stages: (Stream | PipelineStage)[], ps: PipelineState, end: boolean,
                      outerSignal: AbortSignal | undefined): Stream | undefined {
  if (stages.length < 2)
    throw new StreamTypeError("ERR_MISSING_ARGS", 'The "streams" argument must be specified')
  const signal = ps.ac.signal
  const abort = () => finishImpl(ps, new AbortError(undefined, errorOf(outerSignal?.reason)), false)
  let onOuterAbort: ((event: Event) => void) | undefined = undefined
  if (outerSignal != undefined) {
    const listener = (_event: Event) => abort()
    const outer = outerSignal
    onOuterAbort = listener
    outer.addFunctionListener<Event>("abort", listener, { once: true })
    ps.destroys.push(() => outer.removeListenerOf("abort", listener, false))
  }
  void onOuterAbort
  let retStream: Stream | undefined = undefined
  let retIterable: AsyncGenerator<unknown> | undefined = undefined
  for (let i = 0; i < stages.length; i++) {
    const stage = stages[i]
    const reading = i < stages.length - 1
    const writing = i > 0
    const next = i + 1 < stages.length ? stages[i + 1] : undefined
    const stageEnd = reading || end
    const isLastStream = i == stages.length - 1
    if (stage instanceof Stream) {
      const stream = stage
      if (next instanceof Stream && (next.closed || next.destroyed))
        throw new StreamError("ERR_STREAM_UNABLE_TO_PIPE",
                              "Cannot pipe to a closed or destroyed stream")
      if (stageEnd) {
        const cleanup = pipelineDestroyer(stream, reading, writing, ps)
        if (isReadable(stream) && isLastStream) ps.lastStreamCleanup.push(cleanup)
      }
      const onError = function (this: Stream, err: Error) {
        void this
        if (err.name != "AbortError" && codeOf(err) != "ERR_STREAM_PREMATURE_CLOSE")
          finishImpl(ps, err, false)
      }
      stream.on("error", onError, kThisListener)
      if (isReadable(stream) && isLastStream)
        ps.lastStreamCleanup.push(() => stream.removeListener("error", onError))
      if (i == 0) {
        retStream = stream
      } else if (retStream != undefined && isReadableNodeStream(retStream)) {
        ps.finishCount += 2
        const cleanup = pipelinePipe(retStream, stream, ps, stageEnd)
        if (isReadable(stream) && isLastStream) ps.lastStreamCleanup.push(cleanup)
        retStream = stream
      } else if (retIterable != undefined) {
        ps.finishCount++
        void pumpToNode(retIterable, stream, ps, stageEnd)
        retIterable = undefined
        retStream = stream
      } else {
        throw new StreamTypeError(
          "ERR_INVALID_ARG_TYPE",
          'The "val" argument must be an instance of Readable, Iterable, or AsyncIterable.',
        )
      }
      continue
    }
    // A function, or an iterable.
    if (i == 0) {
      const source = stage.source
      if (source == undefined)
        throw new StreamTypeError("ERR_INVALID_ARG_TYPE", "the first stage is no source")
      retIterable = source(signal)
      continue
    }
    const input = retIterable ?? (retStream != undefined ? readableChunks(retStream) : undefined)
    if (input == undefined)
      throw new StreamTypeError("ERR_INVALID_ARG_TYPE", "a stage has nothing before it")
    retStream = undefined
    const transform = stage.transform
    if (transform != undefined && reading) {
      retIterable = transform(input, signal)
      continue
    }
    // The last function: its chunks or its Promise go to a PassThrough,
    // which is the end of the pipeline.
    const pt = new PassThrough<unknown>({ objectMode: true })
    const sink = stage.sink
    if (sink != undefined) {
      ps.finishCount++
      sink(input, signal).then(
        (val: unknown) => {
          ps.value = val
          if (val != undefined) pt._writeAny(val, undefined, nop)
          if (stageEnd) pt._endWith(false, undefined, undefined, undefined)
          process.nextTick(() => pipelineFinish(ps))
        },
        (err: Error) => {
          pt.destroy(err)
          process.nextTick(() => pipelineFinish(ps, err))
        },
      )
    } else if (transform != undefined) {
      ps.finishCount++
      void pumpToNode(transform(input, signal), pt, ps, stageEnd)
    }
    retIterable = undefined
    retStream = pt
    const cleanup = pipelineDestroyer(pt, false, true, ps)
    if (isLastStream) ps.lastStreamCleanup.push(cleanup)
  }
  if (signal.aborted || (outerSignal != undefined && outerSignal.aborted)) process.nextTick(abort)
  return retStream
}

// pipeline(...streams, callback) of node:stream (the compiler calls this):
// gives the last stream.
export function streamPipeline<S extends Stream>(stages: (Stream | PipelineStage)[], last: S,
                                                 callback: StreamCallback): S {
  const ps = new PipelineState((err: Error | undefined, value: unknown) => {
    void value
    callback(err)
  })
  const lastStream: Stream = last
  stages.push(lastStream)
  pipelineImpl(stages, ps, true, undefined)
  return last
}

// The same where the last is a function: gives the PassThrough its result
// goes to.
export function streamPipelineToStage(stages: (Stream | PipelineStage)[],
                                      callback: StreamCallback): Stream {
  const ps = new PipelineState((err: Error | undefined, value: unknown) => {
    void value
    callback(err)
  })
  const last = pipelineImpl(stages, ps, true, undefined)
  if (last == undefined) throw new StreamTypeError("ERR_INVALID_RETURN_VALUE", "no stream")
  return last
}

// pipeline() of node:stream/promises.
export function streamPipelinePromise(stages: (Stream | PipelineStage)[],
                                      options?: { signal?: AbortSignal; end?: boolean }):
    Promise<void> {
  return new Promise<void>((resolve, reject) => {
    const ps = new PipelineState((err: Error | undefined, value: unknown) => {
      void value
      if (err != undefined) reject(err)
      else resolve()
    })
    pipelineImpl(stages, ps, options?.end ?? true, options?.signal)
  })
}

// pipeline() of node:stream/promises whose last stage is a function: the
// value its Promise gives.
export function streamPipelineValue<V>(stages: (Stream | PipelineStage)[],
                                       options?: { signal?: AbortSignal; end?: boolean }):
    Promise<V> {
  return new Promise<V>((resolve, reject) => {
    const ps = new PipelineState((err: Error | undefined, value: unknown) => {
      if (err != undefined) reject(err)
      else resolve(__tz_chunk<V>(value))
    })
    pipelineImpl(stages, ps, options?.end ?? true, options?.signal)
  })
}

// finished(stream, [options], callback) of node:stream (the compiler calls
// this).
export function streamFinished(stream: Stream, options: FinishedOptions | undefined,
                               callback: StreamCallback): () => void {
  return eos(stream, options, callback)
}

// destroy(stream, error) of node:stream.
export function destroy(stream: Stream, error?: Error) {
  destroyer(stream, error)
}

// Stream.isReadable() and the others of node:stream.
export function isReadableStream(stream: Stream): boolean {
  return isReadable(stream)
}

export function isWritableStream(stream: Stream): boolean {
  return isWritable(stream)
}

export function isDestroyedStream(stream: Stream): boolean {
  return isDestroyed(stream)
}

//===----------------------------------------------------------------------===//
// lib/internal/streams/compose.js
//===----------------------------------------------------------------------===//

// The Duplex compose() makes: writes go to the first stream, reads come
// from the last.
class ComposedDuplex<W, R> extends Duplex<W, R> {
  constructor(sides: DuplexSides) {
    pendingSides = sides
    super()
  }


  @__tz_hidden _head: Stream | undefined = undefined
  @__tz_hidden _tail: Stream | undefined = undefined
  @__tz_hidden _ondrain: StreamCallback | undefined = undefined
  @__tz_hidden _onfinish: StreamCallback | undefined = undefined
  @__tz_hidden _onreadable: (() => void) | undefined = undefined
  @__tz_hidden _onclose: StreamCallback | undefined = undefined
  @__tz_hidden _closing = true
  @__tz_hidden _composeListeners: (() => void)[] = []

  _write(chunk: W, encoding: string, callback: StreamCallback): void {
    const head = this._head
    if (head == undefined) return
    if (head._writeAny(chunk, encoding, nop) == true) callback()
    else this._ondrain = callback
  }

  _final(callback: StreamCallback): void {
    const head = this._head
    if (head != undefined) head._endWith(false, undefined, undefined, undefined)
    this._onfinish = callback
  }

  _read(size: int): void {
    void size
    const tail = this._tail
    if (tail == undefined) return
    while (true) {
      const buf = tail._readChunk()
      if (buf == undefined) {
        this._onreadable = () => this._read(0)
        return
      }
      if (!this._pushChunk(buf)) return
    }
  }

  _destroy(error: Error | undefined, callback: StreamCallback): void {
    let err = error
    if (err == undefined && this._closing) err = new AbortError()
    this._onreadable = undefined
    this._ondrain = undefined
    this._onfinish = undefined
    // node leaves its listeners on the first and the last stream; they go
    // here, so that those do not hold this one.
    const listeners = this._composeListeners
    this._composeListeners = []
    for (const remove of listeners) remove()
    const tail = this._tail
    if (tail != undefined) destroyer(tail, err)
    if (!this._closing) callback(err)
    else this._onclose = callback
  }
}

// compose(...streams) of node:stream, of streams (the compiler calls this).
export function streamCompose<W, R>(streams: Stream[]): Duplex<W, R> {
  if (streams.length == 0)
    throw new StreamTypeError("ERR_MISSING_ARGS", 'The "streams" argument must be specified')
  for (let n = 0; n < streams.length; ++n) {
    const stream = streams[n]
    if (stream == undefined) continue
    if (n < streams.length - 1 && !isReadable(stream))
      throw new StreamTypeError(
        "ERR_INVALID_ARG_VALUE",
        `The argument 'streams[${n}]' must be readable. Received ${__tz_lib_class_name(stream)}`,
      )
    if (n > 0 && !isWritable(stream))
      throw new StreamTypeError(
        "ERR_INVALID_ARG_VALUE",
        `The argument 'streams[${n}]' must be writable. Received ${__tz_lib_class_name(stream)}`,
      )
  }
  const first = streams[0]
  const lastOne = streams[streams.length - 1]
  if (first == undefined || lastOne == undefined)
    throw new StreamTypeError("ERR_MISSING_ARGS", 'The "streams" argument must be specified')
  const head = first
  const tail = lastOne
  const writable = isWritable(head)
  const readable = isReadable(tail)
  const sides = new DuplexSides()
  sides.writableObjectMode = head.writableObjectMode
  sides.readableObjectMode = tail.readableObjectMode
  sides.writable = writable
  sides.readable = readable
  sides.hasFinal = true
  const d = new ComposedDuplex<W, R>(sides)
  d._head = head
  d._tail = tail
  const onfinished = (err?: Error) => {
    const cb = d._onclose
    d._closing = false
    d._onclose = undefined
    if (cb != undefined) cb(err)
    else if (err != undefined) d.destroy(err)
    else if (!readable && !writable) d.destroy()
  }
  if (streams.length == 1) {
    onfinished()
  } else {
    // Not streamPipeline(), whose copy for Stream code that no copy of this
    // one reaches would call into code left out.
    const stages: (Stream | PipelineStage)[] = []
    for (const stream of streams) stages.push(stream)
    const ps = new PipelineState((err: Error | undefined, value: unknown) => {
      void value
      onfinished(err)
    })
    pipelineImpl(stages, ps, true, undefined)
  }
  if (writable) {
    const ondrain = function (this: Stream) {
      void this
      const cb = d._ondrain
      if (cb != undefined) {
        d._ondrain = undefined
        cb()
      }
    }
    head.on("drain", ondrain, kThisListener)
    d._composeListeners.push(() => head.removeListener("drain", ondrain))
    const cleanup = eos(tail, undefined, () => {
      const cb = d._onfinish
      if (cb != undefined) {
        d._onfinish = undefined
        cb()
      }
    })
    d._composeListeners.push(cleanup)
  }
  if (readable) {
    const onreadable = function (this: Stream) {
      void this
      const cb = d._onreadable
      if (cb != undefined) {
        d._onreadable = undefined
        cb()
      }
    }
    const onend = function (this: Stream) {
      void this
      d._pushChunk(undefined)
    }
    tail.on("readable", onreadable, kThisListener)
    tail.on("end", onend, kThisListener)
    d._composeListeners.push(() => {
      tail.removeListener("readable", onreadable)
      tail.removeListener("end", onend)
    })
  }
  return d
}

// The chunks of a readable stream as they are: what node:stream/consumers
// reads.
export function streamChunks(stream: Stream): AsyncGenerator<unknown> {
  return createAsyncIterator<unknown>(stream, true)
}
