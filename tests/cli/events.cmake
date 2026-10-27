# node:events in a project: a local package whose emitter gets its event map
# from the emit() calls of the whole program (checked in rounds, docs/events.md),
# the errors of the last round only, and the built-in modules
# written in tz (tz.exe's lib directory) found from any directory.
include(${SOURCE}/../cli.cmake)

write(libs/ticker/tz.json [[
{
  "name": "ticker",
  "exports": { ".": "./mod.tz" }
}
]])
write(libs/ticker/mod.tz [[
import { EventEmitter } from "node:events"

// Its map comes from the emits below and from the program that uses it.
export class Ticker extends EventEmitter {
  count = 0
  tick(by: string) {
    this.count++
    this.emit("tick", this.count, by)
  }
}
]])
tz(init app)
tz(add ../libs/ticker IN app STDOUT "added ticker: file:../libs/ticker\n")
write(app/main.tz [[
import { on, once } from "node:events"
import { Ticker } from "ticker"

const ticker = new Ticker()
let total = 0
ticker.on("tick", (n, by) => {
  const step: int = parseInt(by).unwrapOr(0)
  if (step >= 0 && step < 1000) total += step
  console.log(`tick ${n} by ${by}`)
  if (n == 2) ticker.emit("done", total)
})
setTimeout(() => {
  ticker.tick("1")
  ticker.tick("20")
}, 1)
const [sum] = await once(ticker, "done")
console.log(`done ${sum}`)
setTimeout(() => ticker.emit("done", -1), 1)
for await (const [last] of on(ticker, "done")) {
  console.log(`last ${last}`)
  break
}
]])
tz(run IN app STDOUT "tick 1 by 1\ntick 2 by 20\ndone 21\nlast -1\n")
file(MAKE_DIRECTORY ${WORK}/app/sub)
tz(build -o ../app.exe ../main.tz IN app/sub)

# An overflow found in the last round, reported once.
write(app/sum.tz [[
import { Ticker } from "ticker"
const t = new Ticker()
let total = 0
t.on("tick", (n, by) => {
  total += parseInt(by).unwrapOr(0)
})
t.tick("1")
]])
tz(build sum.tz IN app EXIT 1 STDERR "error: 'total += parseInt(by).unwrapOr(0)' may overflow int")
string(REGEX MATCHALL "may overflow int" errors "${TZ_STDERR}")
list(LENGTH errors count)
if(NOT count EQUAL 1)
  message(FATAL_ERROR "expected one overflow error, found ${count}:\n${TZ_STDERR}")
endif()

# An event that nothing emits, found in the last round, reported once.
write(app/bad.tz [[
import { Ticker } from "ticker"
const t = new Ticker()
t.on("tock", () => console.log("never"))
t.tick("1")
]])
tz(build bad.tz IN app EXIT 1 STDERR "no code of the program emits 'tock' on this emitter; it emits 'tick'")
string(REGEX MATCHALL "emits 'tock'" errors "${TZ_STDERR}")
list(LENGTH errors count)
if(NOT count EQUAL 1)
  message(FATAL_ERROR "expected one error, found ${count}:\n${TZ_STDERR}")
endif()
