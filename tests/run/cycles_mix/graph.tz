// What main.ts builds its cycles from: an emitter whose events are those its
// code and main.ts emit; a server whose connections refer back to it, with
// the signal of its AbortController, the promises of their ends and a hash
// of what each reads; and handlers that a class decorator registers by path,
// which reflection makes for each server.
import { EventEmitter } from "node:events"
import { createHash } from "node:crypto"

export class Room extends EventEmitter {
  lines: string[] = []
  constructor(readonly name: string) {
    super()
  }
  say(who: string, text: string) {
    this.emit("say", who, text)
  }
}

// `@Route("/path")`: the class handles that path.
export const routes = new Map<string, Constructor<Handler>>()
export function Route(path: string) {
  return (cls: Constructor<Handler>) => {
    routes.set(path, cls)
  }
}

export class Handler {
  handled = 0
  constructor(readonly server: Server) {}
  handle(conn: Conn, text: string): string {
    this.handled++
    return `${conn.id}:${text}`
  }
}

export class Conn {
  peer?: Conn
  onData?: (text: string) => void
  readonly finish: (why: string) => void
  readonly done: Promise<string>
  readonly hash = createHash("sha256")
  constructor(readonly server: Server, readonly id: int) {
    // The resolve of the promise, whose environment is the promise.
    const { promise, resolve } = Promise.withResolvers<string>()
    this.done = promise
    this.finish = resolve
  }
  receive(text: string) {
    this.hash.update(text)
    this.onData?.(text)
  }
  close(why: string) {
    this.server.conns.delete(this)
    this.finish(`${this.id} ${why}`)
  }
}

export class Server {
  readonly conns = new Set<Conn>()
  readonly byId = new Map<int, Conn>()
  readonly handlers = new Map<string, Handler>()
  readonly log: string[] = []
  readonly controller = new AbortController()
  constructor(readonly name: string) {
    // The signal's listener holds the server, which holds the signal.
    this.controller.signal.addEventListener("abort", () => {
      for (const c of [...this.conns]) c.close("shutdown")
    })
    for (const [path, cls] of routes) this.handlers.set(path, Reflect.construct(cls, [this]))
  }
  accept(id: int): Conn {
    const c = new Conn(this, id)
    this.conns.add(c)
    this.byId.set(id, c)
    return c
  }
  route(path: string, conn: Conn, text: string): string {
    return this.handlers.get(path)?.handle(conn, text) ?? `no route ${path}`
  }
}
