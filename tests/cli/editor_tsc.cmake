# Programs of tz as TypeScript's checker sees them in the editor (docs/modules.md,
# section 6): a project that tz init set up, using what tz has built in, a
# package and node's modules, and the examples build with tz and have no
# errors for tsc. Needs node and TypeScript's lib/typescript.js (TZ_TYPESCRIPT,
# by default VS Code's).
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

write(libs/shapes/tz.json [[
{
  "name": "shapes",
  "exports": { ".": "./mod.ts", "./units": "./units.ts" }
}
]])
write(libs/shapes/mod.ts [[
export function area(w: double, h: double) { return w * h }
]])
write(libs/shapes/units.ts [[
export const cm = "cm"
]])
tz(init app)
tz(add ../libs/shapes IN app)
write(app/config.json [[
{"port": 8080, "tags": ["a"]}
]])
write(app/note.txt "hello")
write(app/main.ts [=[
import { readFileSync } from "node:fs"
import { join } from "node:path"
import { readLine } from "tz:io"
import { serve } from "tz:http"
import { area } from "shapes"
import { cm } from "shapes/units"

// Numbers.
const n: int = 3
const big: long = 1 << 20
const price: decimal = 19.99
const total = price * n + 0.01
console.log(total, total.toFixed(1), total.round(0, "half-even"), decimal("0.1") + decimal("0.2"))
console.log(decimal.max(price, total), big, area(2, 1.5), cm)

// JSON.
type Config = { port: double; tags: string[] }
const config = JSON.parse<Config>(embedText("./config.json"))
const loose = JSON.parse("[1, 2]")
const json: Json = embedJson("./config.json")
console.log(config.port, config.tags, loose[1], fromJson<Config>(json).port, embedBytes("./note.txt").length)

// Results.
function half(x: int): Result<int, string> {
  return x % 2 == 0 ? ok(x / 2 | 0) : err(`${x} is odd`)
}
console.log(half(4).match(v => `half ${v}`, e => e), half(3).isErr())
const read = Result.fromThrowable((file: string) => readFileSync(file, "utf8"))
console.log(read(join("no", "file")).mapErr(e => e.message.length > 0).isErr())
const both = safeTry(function* () {
  const a = yield* half(8)
  const b = yield* half(a)
  return ok(a + b)
})
console.log(both.isOk())
const later = await ResultAsync.fromPromise(Promise.resolve(5), e => e.message)
console.log(later.isOk(), (await okAsync(1)).isOk())

// Errors of the system.
try {
  readFileSync("missing.txt", "utf8")
} catch (e) {
  if (e instanceof SystemError) console.log(e.code, e.syscall)
}
function status(e: Error) {
  return e instanceof ExecSyncError ? e.status : undefined
}
console.log(status(new Error("x")))

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
const answer = await fetch(`http://127.0.0.1:${server.port}/hi`)
console.log(answer.status, await answer.text())
await server.stop()
]=])
tz(run IN app STDOUT [[
59.98 60.0 60 0.3
59.98 1048576 3 cm
8080 [ 'a' ] 2 8080 5
half 2 true
true
true
true true
ENOENT open
undefined
call twice
8 1 rows
no input
200 got /hi
]])
tsc(app/tsconfig.json)

# The examples, with the settings next to tz.
get_filename_component(types ${TZ} DIRECTORY)
get_filename_component(examples ${SOURCE}/../../examples ABSOLUTE)
write(examples/tsconfig.json "{
  \"extends\": \"${types}/types/tsconfig.json\",
  \"include\": [\"${examples}/*.ts\"]
}
")
tsc(examples/tsconfig.json)
