# The declarations next to tz for TypeScript's checker (editors/types, docs/modules.md,
# section 6), for a plugin of tsserver to come: a program that uses what tz
# has built in and node's modules, and the examples, build with tz and, as
# .ts copies, have no errors for tsc. Needs node and TypeScript's
# lib/typescript.js (TZ_TYPESCRIPT, by default VS Code's).
include(${SOURCE}/../cli.cmake)
if(NOT NODE)
  skip("node is not installed")
endif()
if(NOT EXISTS "${TYPESCRIPT}")
  skip("TypeScript is not found; set TZ_TYPESCRIPT to its lib/typescript.js")
endif()

# tsc(<tsconfig.json>): no errors in the files it names (relative to WORK).
function(tsc config)
  execute_process(COMMAND ${NODE} ${SOURCE}/tsc.cjs ${TYPESCRIPT} ${WORK}/${config}
    OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE code)
  if(NOT code EQUAL 0)
    message(FATAL_ERROR "tsc finds errors with ${config}:\n${out}${err}")
  endif()
endfunction()

write(app/config.json [[
{"port": 8080, "tags": ["a"]}
]])
write(app/note.txt "hello")
set(program [=[
import { readFileSync } from "node:fs"
import { execFileSync } from "node:child_process"
import { readLine } from "tz:io"
import { Server } from "tz:http"

// Numbers.
const n: int = 3
const big: int = 1 << 20
const price: decimal = 19.99
const total = price * n.toDecimal() + 0.01
console.log(total, total.toFixed(1), total.round(0, "half-even"), decimal("0.1") + decimal("0.2"))
console.log(decimal.max(price, total), big)

// JSON.
type Config = { port: double; tags: string[] }
const config = JSON.parse<Config>(embedText("./config.json"))
const loose = JSON.parse("[1, 2]").unwrapOr(null)
const json: Json = embedJson("./config.json")
if (config.isOk()) console.log(config.value.port, config.value.tags, loose[1], fromJson<Config>(json).map(c => c.port).unwrapOr(0), embedBytes("./note.txt").length)

// Results.
function half(x: int): Result<int, string> {
  return x % 2 == 0 ? ok(x / 2) : err(`${x} is odd`)
}
console.log(half(4).match(v => `half ${v}`, e => e), half(3).isErr())
function quarter(x: int) {
  const h = half(x)
  if (h.isErr()) return h
  return half(h.value)
}
console.log(quarter(8).isOk(), quarter(6).mapErr(e => e.length).isErr())
async function later(x: int) {
  return half(x)
}
console.log(Result.combine(await Promise.all([2, 4].map(later))).map(xs => xs.length).isOk())

// Errors of the system.
const missing = readFileSync("missing.txt", "utf8")
if (missing.isErr()) console.log(missing.error.code, missing.error.syscall)
const listed = execFileSync("tz-no-such-program", ["--version"], { encoding: "utf8" })
console.log(listed.match(out => out, e => e.code))

// Decorators and metadata.
function Table(name: string) {
  return (cls: object) => Reflect.defineMetadata(Table, name, cls)
}
function Log<A extends unknown[], R>(_cls: object, name: string, method: MethodDescriptor<A, R>) {
  const original = method.value
  if (original != undefined)
    method.value = function (this: unknown, ...args: A): R {
      console.log("call", name)
      return original.apply(this, args)
    }
}
let made = 0
function Counted<T extends Constructor>(cls: T) {
  return class extends cls {
    constructor(...args: ConstructorArgs) {
      super(...args)
      made++
    }
  }
}
@Table("rows")
@Counted
class Row {
  constructor(public id: int) {}
  @Log
  twice() { return this.id * 2 }
}
console.log(new Row(4).twice(), made, Reflect.getMetadata(Table, Row) ?? "none")

// Timers, input and the network.
const timer: Timeout = setTimeout(() => console.log("never"), 1000)
clearTimeout(timer)
console.log(readLine() ?? "no input")
const server = new Server(request => new Response(`got ${new URL(request.url).pathname}`))
const listened = server.listen({ hostname: "127.0.0.1", port: 0 })
if (listened.isOk()) {
  console.log(server.port > 0)
  await server.stop()
}
]=])
write(app/main.tz "${program}")
tz(run main.tz IN app STDOUT [[
59.98 60.0 60 0.3
59.98 1048576
8080 [ 'a' ] 2 8080 5
half 2 true
true true
true
ENOENT open
ENOENT
call twice
8 1 rows
no input
true
]])

# The web, where tz gives a Result and methods and node gives exceptions,
# null and properties: tz changes those declarations in its copy of
# @types/node (editors/types/patch.cmake).
set(web [=[
import { Server } from "tz:http"

// The web: fetch and reading a body give a Result; so does URL.parse. A
// status written in the code is an HttpStatus; one from a variable becomes
// one after isHttpStatus.
const code = 200 + (process.argv.length > 99 ? 1 : 0)
const server = new Server(request => {
  const path = new URL(request.url).pathname
  if (path == "/away") return Response.redirect(new URL("/here", request.url), 307)
  if (isHttpStatus(code)) return new Response(`got ${path}`, { status: code })
  return new Response("none", { status: 500 })
})
const listened = server.listen({ hostname: "127.0.0.1", port: 0 })
if (listened.isOk()) {
  const base = listened.value
  const res = await fetch(new URL("/away", base))
  if (res.isOk()) {
    const response = res.value
    console.log(response.status, response.redirected, response.bodyUsed())
    const text = await response.text()
    console.log(text.unwrapOr("-"), response.bodyUsed())
  }
  const url = URL.parse("/a?b=1", base)
  console.log(url.isOk() ? url.value.pathname : "bad", URL.parse("no url").isErr())
  if (url.isOk()) {
    url.value.searchParams.replace("c=2")
    console.log(url.value.href.endsWith("/a?c=2"), url.value.setPort(process.argv[2] ?? "x").isErr())
  }
  await server.stop()
}

// A stream of the web reads with a Result too.
const stream = new ReadableStream<string>({
  start: controller => {
    console.log(controller.desiredSize())
    controller.enqueue("a")
    controller.close()
  },
})
const reader = stream.getReader()
const first = await reader.read()
console.log(first.isOk() && !first.value.done ? first.value.value : "-", stream.disturbed)
reader.releaseLock()
for await (const chunk of new ReadableStream<string>({ start: c => { c.enqueue("b"); c.close() } }))
  console.log(chunk.unwrapOr("-"))
const ping = new Event("ping")
console.log(ping.immediateStopped)
const when = Date.parse("2026-10-28T10:00Z")
const price = decimal(process.argv[3] ?? "1.5")
console.log(when.isOk(), price.isOk() ? price.value.toFixed(1) : "-", decimal("2.5").toFixed(1))
]=])
write(app/web.tz "${web}")
tz(run web.tz IN app STDOUT [[
200 true false
got /here true
/a true
true true
1
a true
b
false
true 1.5 2.5
]])

# The programs and the examples as .ts files, with the settings next to tz.
get_filename_component(types ${TZ} DIRECTORY)
write(tsc/main.ts "${program}")
write(tsc/web.ts "${web}")
file(GLOB examples ${SOURCE}/../../examples/*.tz)
foreach(example IN LISTS examples)
  get_filename_component(name ${example} NAME_WE)
  configure_file(${example} ${WORK}/tsc/${name}.ts COPYONLY)
endforeach()
write(tsc/tsconfig.json "{
  \"extends\": \"${types}/types/tsconfig.json\",
  \"include\": [\"*.ts\"]
}
")
tsc(tsc/tsconfig.json)
