// The streams of node:fs and of process: fs.ReadStream and fs.WriteStream,
// ported from lib/internal/fs/streams.js of Node 24 (docs/streams.md, step
// 3), process.stdin as a Readable and process.stdout and process.stderr as
// Writables. The calls of the system are the runtime's (runtime/fs.c), each
// a work for the pool of runtime/io.c, as a call of node:fs with a callback
// is: `__tz_fsOpen` and the others give a Promise, which the code waits for
// as node's callback is called.

import { AbortSignal } from "tz:web"
import { Readable, Stream, StreamCallback, Writable, WriteChunk, streamFinished } from "node:stream"

// node's ERR_OUT_OF_RANGE.
class FsRangeError extends RangeError {
  code = "ERR_OUT_OF_RANGE"
}

// node's ERR_STREAM_DESTROYED.
class FsStreamError extends Error {
  code: string
  constructor(code: string, message: string) {
    super(message)
    this.code = code
  }
}

function validateInteger(value: double, name: string) {
  if (!Number.isInteger(value) || value < 0 || value > Number.MAX_SAFE_INTEGER)
    throw new FsRangeError(
      `The value of "${name}" is out of range. It must be >= 0 && <= 9007199254740991. ` +
        `Received ${value}`,
    )
}

export interface ReadStreamOptions {
  flags?: string
  encoding?: string
  fd?: int
  mode?: int
  autoClose?: boolean
  emitClose?: boolean
  start?: double
  end?: double
  highWaterMark?: double
  signal?: AbortSignal
}

export interface WriteStreamOptions {
  flags?: string
  encoding?: string
  fd?: int
  mode?: int
  autoClose?: boolean
  emitClose?: boolean
  start?: double
  flush?: boolean
  highWaterMark?: double
  signal?: AbortSignal
}

// The descriptor of a stream, closed when it is destroyed (node's close()),
// fsync'ed first when it flushes.
function closeFd(fd: int | undefined, flush: boolean, err: Error | undefined,
                 cb: StreamCallback) {
  if (fd == undefined) {
    cb(err)
    return
  }
  const open = fd
  const closeIt = (error: Error | undefined) => {
    __tz_fsClose(open, false).then(
      () => cb(error),
      (er: Error) => cb(er),
    )
  }
  if (flush)
    __tz_fsClose(open, true).then(
      () => closeIt(err),
      (er: Error) => closeIt(err ?? er),
    )
  else closeIt(err)
}

// fs.ReadStream: a Readable of the bytes of a file, from `start` to `end`
// (both included), in chunks of highWaterMark (64 KiB) at most.
export class ReadStream<T = Buffer> extends Readable<T> {
  @__tz_hidden fd: int | undefined
  path: string
  flags: string
  mode: int
  start: double | undefined
  end: double
  pos: double | undefined
  bytesRead = 0
  @__tz_hidden pending: boolean
  @__tz_hidden autoClose: boolean
  @__tz_hidden _performingIO = false
  @__tz_hidden _ioDone: ((error: Error | undefined) => void) | undefined = undefined

  constructor(path: string, options?: ReadStreamOptions) {
    super({
      highWaterMark: options?.highWaterMark ?? 64 * 1024,
      encoding: options?.encoding,
      emitClose: options?.emitClose,
      autoDestroy: options?.autoClose ?? true,
      signal: options?.signal,
    })
    this.fd = options?.fd
    this.path = path
    this.flags = options?.flags ?? "r"
    this.mode = options?.mode ?? 0o666
    this.autoClose = options?.autoClose ?? true
    const start = options?.start
    this.start = start
    this.pos = start
    if (start != undefined) validateInteger(start, "start")
    const end = options?.end ?? Infinity
    this.end = end
    if (end != Infinity) {
      validateInteger(end, "end")
      if (start != undefined && start > end)
        throw new FsRangeError(
          `The value of "start" is out of range. It must be <= "end" (here: ${end}). ` +
            `Received ${start}`,
        )
    }
    this.pending = this.fd == undefined
  }

  _construct(callback: StreamCallback): void {
    if (this.fd != undefined) {
      callback()
      return
    }
    __tz_fsOpen(this.path, this.flags, this.mode).then(
      (fd: int) => {
        this.fd = fd
        this.pending = false
        callback()
        this.emit("open", fd)
        this.emit("ready")
      },
      (er: Error) => callback(er),
    )
  }

  _read(size: int): void {
    const pos = this.pos
    const left = pos != undefined ? this.end - pos + 1 : this.end - this.bytesRead + 1
    const n = left < size ? Math.floor(left) | 0 : size
    const fd = this.fd
    if (n <= 0 || fd == undefined) {
      this._pushChunk(undefined)
      return
    }
    const buf = Buffer.allocUnsafeSlow(n)
    this._performingIO = true
    __tz_fsRead(fd, buf, 0, n, pos ?? -1).then(
      (bytesRead: int) => this.#afterRead(undefined, bytesRead, buf),
      (er: Error) => this.#afterRead(er, 0, buf),
    )
  }

  #afterRead(er: Error | undefined, bytesRead: int, buf: Buffer) {
    this._performingIO = false
    if (this.destroyed) {
      const done = this._ioDone
      this._ioDone = undefined
      if (done != undefined) done(er)
      return
    }
    if (er != undefined) {
      this.destroy(er)
    } else if (bytesRead > 0) {
      const pos = this.pos
      if (pos != undefined) this.pos = pos + bytesRead
      this.bytesRead += bytesRead
      let chunk = buf
      if (bytesRead != buf.length) {
        const dst = Buffer.allocUnsafeSlow(bytesRead)
        buf.copy(dst, 0, 0, bytesRead)
        chunk = dst
      }
      this._pushChunk(chunk)
    } else {
      this._pushChunk(undefined)
    }
  }

  _destroy(error: Error | undefined, callback: StreamCallback): void {
    const finish = (er: Error | undefined) => {
      const fd = this.fd
      this.fd = undefined
      closeFd(fd, false, er, callback)
    }
    if (this._performingIO) this._ioDone = (er: Error | undefined) => finish(error ?? er)
    else finish(error)
  }

  close(callback?: StreamCallback) {
    if (callback != undefined) streamFinished(this, undefined, callback)
    this.destroy()
  }
}

// fs.WriteStream: a Writable of a file, from `start` on, or where the flags
// say (the end, with "a").
export class WriteStream extends Writable<Buffer> {
  @__tz_hidden fd: int | undefined
  path: string
  flags: string
  mode: int
  start: double | undefined
  pos: double | undefined
  bytesWritten = 0
  flush: boolean
  @__tz_hidden pending: boolean
  @__tz_hidden autoClose: boolean
  @__tz_hidden _performingIO = false
  @__tz_hidden _ioDone: ((error: Error | undefined) => void) | undefined = undefined

  constructor(path: string, options?: WriteStreamOptions) {
    super({
      highWaterMark: options?.highWaterMark,
      decodeStrings: true,
      emitClose: options?.emitClose,
      autoDestroy: options?.autoClose ?? true,
      signal: options?.signal,
      defaultEncoding: options?.encoding,
    })
    this.fd = options?.fd
    this.path = path
    this.flags = options?.flags ?? "w"
    this.mode = options?.mode ?? 0o666
    this.autoClose = options?.autoClose ?? true
    this.flush = options?.flush ?? false
    const start = options?.start
    this.start = start
    this.pos = start
    if (start != undefined) validateInteger(start, "start")
    this.pending = this.fd == undefined
  }

  _construct(callback: StreamCallback): void {
    if (this.fd != undefined) {
      callback()
      return
    }
    __tz_fsOpen(this.path, this.flags, this.mode).then(
      (fd: int) => {
        this.fd = fd
        this.pending = false
        callback()
        this.emit("open", fd)
        this.emit("ready")
      },
      (er: Error) => callback(er),
    )
  }

  // node's writeAll: the whole of `data`, from its byte `offset` on, in as
  // many writes as the system takes.
  #writeAll(data: Buffer, offset: int, size: int, pos: double | undefined, cb: StreamCallback,
            retries: int) {
    const fd = this.fd
    if (fd == undefined) {
      cb(new FsStreamError("ERR_STREAM_DESTROYED", "Cannot call write after a stream was destroyed"))
      return
    }
    __tz_fsWrite(fd, data, offset, size, pos ?? -1).then(
      (bytesWritten: int) => {
        if (this.destroyed) {
          cb(new FsStreamError("ERR_STREAM_DESTROYED",
                               "Cannot call write after a stream was destroyed"))
          return
        }
        this.bytesWritten += bytesWritten
        const tries = bytesWritten != 0 ? 0 : retries + 1
        const left = size - bytesWritten
        if (tries > 5) cb(new FsStreamError("ERR_SYSTEM_ERROR", "write failed"))
        else if (left != 0)
          this.#writeAll(data, offset + bytesWritten, left,
                         pos != undefined ? pos + bytesWritten : undefined, cb, tries)
        else cb()
      },
      (er: Error) => cb(er),
    )
  }

  _write(chunk: Buffer, encoding: string, callback: StreamCallback): void {
    void encoding
    this._performingIO = true
    this.#writeAll(chunk, 0, chunk.length, this.pos, (er?: Error) => {
      this._performingIO = false
      if (this.destroyed) {
        callback(er)
        const done = this._ioDone
        this._ioDone = undefined
        if (done != undefined) done(er)
        return
      }
      callback(er)
    }, 0)
    const pos = this.pos
    if (pos != undefined) this.pos = pos + chunk.length
  }

  // node's _writev: the chunks as one write.
  _writev(chunks: WriteChunk<Buffer>[], callback: StreamCallback): void {
    const all: Buffer[] = []
    for (const c of chunks) all.push(c.chunk)
    this._write(Buffer.concat(all), "buffer", callback)
  }

  _destroy(error: Error | undefined, callback: StreamCallback): void {
    const finish = (er: Error | undefined) => {
      const fd = this.fd
      this.fd = undefined
      closeFd(fd, this.flush, er, callback)
    }
    if (this._performingIO) this._ioDone = (er: Error | undefined) => finish(error ?? er)
    else finish(error)
  }

  close(callback?: StreamCallback) {
    if (callback != undefined) {
      const done = callback
      if (this.closed) {
        process.nextTick(() => done())
        return
      }
      this.on("close", () => done())
    }
    if (!this.autoClose) this.on("finish", () => this.destroy())
    this._endWith(false, undefined, undefined, undefined)
  }
}

// fs.createReadStream(path, options) and createWriteStream(); the compiler
// gives the encoding written in place of the options apart, and the chunk
// type (src/sema/sema_stream.cpp).
export function createReadStream() {}
export function createWriteStream() {}

export function streamReadFile<T>(path: string, options: ReadStreamOptions | undefined,
                                  encoding: string | undefined): ReadStream<T> {
  if (encoding == undefined) return new ReadStream<T>(path, options)
  return new ReadStream<T>(path, {
    flags: options?.flags,
    encoding,
    fd: options?.fd,
    mode: options?.mode,
    autoClose: options?.autoClose,
    emitClose: options?.emitClose,
    start: options?.start,
    end: options?.end,
    highWaterMark: options?.highWaterMark,
    signal: options?.signal,
  })
}

export function streamWriteFile(path: string, options: WriteStreamOptions | undefined,
                                encoding: string | undefined): WriteStream {
  if (encoding == undefined) return new WriteStream(path, options)
  return new WriteStream(path, {
    flags: options?.flags,
    encoding,
    fd: options?.fd,
    mode: options?.mode,
    autoClose: options?.autoClose,
    emitClose: options?.emitClose,
    start: options?.start,
    flush: options?.flush,
    highWaterMark: options?.highWaterMark,
    signal: options?.signal,
  })
}

//===----------------------------------------------------------------------===//
// process.stdin, process.stdout and process.stderr
//===----------------------------------------------------------------------===//

// process.stdin: the bytes of stdin, from the buffer readLine and readline
// read too. A read waits for the next chunk of input.
class StdinStream extends Readable<Buffer> {
  @__tz_hidden fd = 0
  #reading = false

  _read(size: int): void {
    if (this.#reading) return
    this.#reading = true
    void __tz_stdinChunk(size).then((chunk: Buffer | undefined) => {
      this.#reading = false
      this._pushChunk(chunk)
    })
  }
}

// process.stdout and process.stderr: what is written goes where
// console.log prints, in the order written, at once, as node's
// SyncWriteStream does for a file.
class StdioStream extends Writable<Buffer> {
  constructor(readonly fd: int) {
    super({ emitClose: false, autoDestroy: false })
  }

  _write(chunk: Buffer, encoding: string, callback: StreamCallback): void {
    void encoding
    __tz_stdWrite(this.fd, chunk)
    callback()
  }
}

let stdinStream: StdinStream | undefined = undefined
let stdoutStream: StdioStream | undefined = undefined
let stderrStream: StdioStream | undefined = undefined

// The streams of process (the compiler calls these): each made once, when
// the program first uses it.
export function processStdin(): Readable<Buffer> {
  const made = stdinStream
  if (made != undefined) return made
  const stream = new StdinStream({ highWaterMark: 64 * 1024 })
  stdinStream = stream
  return stream
}

export function processStdout(fd: int): Writable<Buffer> {
  const made = fd == 2 ? stderrStream : stdoutStream
  if (made != undefined) return made
  const stream = new StdioStream(fd)
  if (fd == 2) stderrStream = stream
  else stdoutStream = stream
  stream._markStdio()
  return stream
}

export function isStdioStream(stream: Stream): boolean {
  return stream == stdoutStream || stream == stderrStream
}
