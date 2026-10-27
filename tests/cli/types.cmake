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
import { serve } from "tz:http"

// Numbers.
const n: int = 3
const big: int = 1 << 20
const price: decimal = 19.99
const total = price * n + 0.01
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
  return x % 2 == 0 ? ok(x / 2 | 0) : err(`${x} is odd`)
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
const server = serve({ hostname: "127.0.0.1", port: 0 }, request => new Response(`got ${new URL(request.url).pathname}`))
if (server.isOk()) {
  console.log(server.value.port > 0)
  await server.value.stop()
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

# The program and the examples as .ts files, with the settings next to tz.
get_filename_component(types ${TZ} DIRECTORY)
write(tsc/main.ts "${program}")
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
