// What may run in parallel in a server with the rest of the language
// (docs/async.md, sections 4 and 5): the handler that serve() runs for each
// request, in many copies at once, keeps node's order only where it touches
// the Map that the copies share; a client fetches in parallel, each task
// with a Response of its own; an emitter runs its listener inside emit();
// and objects that form cycles (sessions that know their registry and each
// other) are data like any other.
import { serve } from "tz:http"
import { EventEmitter } from "node:events"

class Stats extends EventEmitter {
  served = 0
  hit(path: string) {
    this.emit("hit", path)
  }
}

class Session {
  peer?: Session
  constructor(readonly owner: Registry, readonly path: string) {}
}

class Registry {
  sessions = new Map<string, Session>()
  open(path: string): Session {
    const session = new Session(this, path)
    const last = this.sessions.get(path)
    if (last != undefined) {
      session.peer = last
      last.peer = session
    }
    this.sessions.set(path, session)
    return session
  }
}

const stats = new Stats()
stats.on("hit", (_path: string) => {
  stats.served++
})
const counts = new Map<string, int>()
const registry = new Registry()
const controller = new AbortController()
const server = serve({ hostname: "127.0.0.1", port: 0, signal: controller.signal }, async (req) => {
  const path = new URL(req.url).pathname
  await Promise.resolve()
  counts.set(path, (counts.get(path) ?? 0) + 1)
  stats.hit(path)
  const session = registry.open(path)
  return new Response(`${path} ${session.peer == undefined ? "first" : "again"}`)
})

async function get(path: string) {
  const res = await fetch(`http://127.0.0.1:${server.port}${path}`)
  return await res.text()
}

const texts = await Promise.all(["/a", "/b", "/c"].map(get))
console.log(texts.join(", "))
console.log(await get("/a"))
const seen = [...counts.keys()].sort().map((k) => `${k}=${counts.get(k) ?? 0}`)
console.log(seen.join(" "), stats.served, registry.sessions.size)
controller.abort()
