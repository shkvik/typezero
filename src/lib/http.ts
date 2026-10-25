// tz:http (docs/net.md, section 1): `serve(options, handler)`, a server of
// HTTP/1.1 whose handler gets a Request and gives a Response, as in Deno and
// Bun, and as hono's `app.fetch` is.
//
// Each connection is an async task that reads its requests one after the
// other (keep-alive, pipelining); the requests of different connections are
// handled at the same time. For now on the one thread of the loop: the
// parallel runtime (docs/async.md, step 4) will run them on all the cores.
// The bytes go through the runtime (runtime/net.c, http.c).

import {
  Request, Response, ReadableStream, SEND_BYTES, SEND_NO_BODY, SEND_STREAM,
  __serverRequest, __responseBody, __bodyKind, __bodyText, __bodyBytes, __bodyStream,
} from "tz:fetch"
import { URL } from "tz:url"
import { AbortSignal, Event } from "tz:web"

function __tz_net_listen(host: string, port: int, maxBody: double): object
function __tz_net_listener_port(listener: object): int
function __tz_net_accept(listener: object): Promise<object | undefined>
function __tz_net_close_listener(listener: object): void
function __tz_net_close(conn: object): void
function __tz_net_sweep(listener: object, idle: double): int
function __tz_http_next(conn: object): Promise<int>
function __tz_http_method(conn: object): string
function __tz_http_target(conn: object): string
function __tz_http_headers(conn: object): string[]
function __tz_http_has_body(conn: object): boolean
function __tz_http_keep_alive(conn: object): boolean
function __tz_http_end_after(conn: object): void
function __tz_http_respond(conn: object, status: int, statusText: string, headers: string[],
                           text: string, bytes: Uint8Array | undefined, flags: int): void
function __tz_http_write(conn: object, chunk: Uint8Array): Promise<void>
function __tz_http_end(conn: object): void

// What onListen gets.
export interface ServeAddress {
  hostname: string
  port: int
}

export interface ServeOptions {
  port?: int
  hostname?: string
  signal?: AbortSignal
  onListen?: (address: ServeAddress) => void
  // A request with a larger body is answered with 413, as in Bun.
  maxBodySize?: double
}

export type Handler = (request: Request) => Response | Promise<Response>

// How long a connection may wait for its next request: node's
// keepAliveTimeout.
const KEEP_ALIVE_MS = 5000

const BODY_TEXT = 1
const BODY_BYTES = 2
const BODY_STREAM = 3
const BODY_WIRE = 4

export class Server {
  #listener: object
  #handler: Handler
  #hostname: string
  #port: int
  #base: string
  #stopping = false
  #open = 0
  #finished: Promise<void>
  #resolveFinished: () => void
  #sweep: Timeout | undefined = undefined

  constructor(listener: object, hostname: string, handler: Handler) {
    this.#listener = listener
    this.#handler = handler
    this.#hostname = hostname
    this.#port = __tz_net_listener_port(listener)
    const shown = hostname.includes(":") ? "[" + hostname + "]" : hostname
    this.#base = "http://" + shown + ":" + String(this.#port)
    const { promise, resolve } = Promise.withResolvers<void>()
    this.#finished = promise
    this.#resolveFinished = resolve
  }

  get port(): int {
    return this.#port
  }

  get hostname(): string {
    return this.#hostname
  }

  get url(): URL {
    const any = this.#hostname == "0.0.0.0" || this.#hostname == "::"
    const host = any ? "localhost" : this.#hostname
    return new URL("http://" + (host.includes(":") ? "[" + host + "]" : host) + ":" +
                   String(this.#port) + "/")
  }

  // Resolved once the server has stopped and its last response is out.
  get finished(): Promise<void> {
    return this.#finished
  }

  // Stops taking connections, ends those that wait for a request, and
  // resolves once the requests being handled are answered.
  async stop(): Promise<void> {
    if (!this.#stopping) {
      this.#stopping = true
      // The timer holds the server through its function: both go.
      const sweep = this.#sweep
      if (sweep != undefined) clearInterval(sweep)
      this.#sweep = undefined
      __tz_net_close_listener(this.#listener)
      __tz_net_sweep(this.#listener, 0)
      if (this.#open == 0) this.#resolveFinished()
    }
    await this.#finished
  }

  __start() {
    const sweep = setInterval(() => {
      __tz_net_sweep(this.#listener, KEEP_ALIVE_MS)
    }, 1000)
    sweep.unref()
    this.#sweep = sweep
    void this.#accept()
  }

  async #accept() {
    for (;;) {
      const conn = await __tz_net_accept(this.#listener)
      if (conn == undefined) break
      void this.#serve(conn)
    }
  }

  async #serve(conn: object) {
    this.#open++
    try {
      for (;;) {
        const got = await __tz_http_next(conn)
        if (got == 0) break
        const request = __serverRequest(conn, __tz_http_method(conn), __tz_http_target(conn),
                                        __tz_http_headers(conn), this.#base,
                                        __tz_http_has_body(conn))
        let response: Response
        try {
          response = await this.#handler(request)
        } catch (e) {
          // As Deno and Bun: the error goes to stderr, the client gets 500.
          console.error(e)
          response = new Response("Internal Server Error", { status: 500 })
        }
        if (this.#stopping) __tz_http_end_after(conn)
        const head = request.method == "HEAD"
        if (!respond(conn, response, head)) await respondStreaming(conn, response, head)
        if (!__tz_http_keep_alive(conn)) break
      }
    } catch (e) {
      // The connection broke (a body that failed as it streamed): it ends.
    } finally {
      __tz_net_close(conn)
      this.#open--
      if (this.#stopping && this.#open == 0) this.#resolveFinished()
    }
  }
}

// Writes a response whose body is all there; false for one that streams.
function respond(conn: object, response: Response, head: boolean): boolean {
  const body = __responseBody(response)
  const kind = __bodyKind(body)
  if (kind == BODY_STREAM || kind == BODY_WIRE) return false
  const flags = (head ? SEND_NO_BODY : 0) | (kind == BODY_BYTES ? SEND_BYTES : 0)
  __tz_http_respond(conn, response.status, response.statusText, response.headers.__flat(),
                    kind == BODY_TEXT ? __bodyText(body) : "", __bodyBytes(body), flags)
  return true
}

// A body that streams goes out as it comes, in chunks.
async function respondStreaming(conn: object, response: Response, head: boolean) {
  const body = __responseBody(response)
  __tz_http_respond(conn, response.status, response.statusText, response.headers.__flat(), "",
                    undefined, SEND_STREAM | (head ? SEND_NO_BODY : 0))
  const stream: ReadableStream<Uint8Array> | undefined = __bodyStream(body)
  if (stream != undefined && !head) {
    const reader = stream.getReader()
    for (;;) {
      let chunk: Uint8Array
      try {
        const r = await reader.read()
        if (r.done) break
        chunk = r.value
      } catch (e) {
        // The stream failed: the response cannot be completed, so the
        // client sees the connection end, and the error goes to stderr.
        console.error(e)
        __tz_http_end_after(conn)
        __tz_net_close(conn)
        return
      }
      try {
        await __tz_http_write(conn, chunk)
      } catch (e) {
        // The client went away: the stream is cancelled, quietly.
        await reader.cancel()
        __tz_http_end_after(conn)
        return
      }
    }
  }
  __tz_http_end(conn)
}

// Starts a server: on `hostname` (0.0.0.0) and `port` (8000, 0 for any
// free one, which server.port then tells), handling each request with
// `handler`.
export function serve(options: ServeOptions, handler: Handler): Server {
  const hostname = options.hostname ?? "0.0.0.0"
  const listener = __tz_net_listen(hostname, options.port ?? 8000,
                                   options.maxBodySize ?? 128 * 1024 * 1024)
  const server = new Server(listener, hostname, handler)
  server.__start()
  // The signal stops the server, with a listener of its own: the program's
  // onabort stays the program's.
  const signal = options.signal
  if (signal != undefined) {
    if (signal.aborted) void server.stop()
    else signal.addFunctionListener<Event>("abort", (_event: Event) => {
      void server.stop()
    }, { once: true })
  }
  options.onListen?.({ hostname, port: server.port })
  return server
}
