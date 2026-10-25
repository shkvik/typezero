# node:events in a project: a local package whose emitter gets its event map
# from the emit() calls of the whole program (checked in rounds, docs/events.md),
# the warnings and errors of the last round only, and the built-in modules
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
  const step: int = parseInt(by)
  total += step
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
tz(run IN app STDOUT "tick 1 by 1\ntick 2 by 20\ndone 21\nlast -1\n"
   STDERR "warning: 'total += step' may overflow int")
# The warning of the last round only, once.
string(REGEX MATCHALL "may overflow int" warnings "${TZ_STDERR}")
list(LENGTH warnings count)
if(NOT count EQUAL 1)
  message(FATAL_ERROR "expected one overflow warning, found ${count}:\n${TZ_STDERR}")
endif()
file(MAKE_DIRECTORY ${WORK}/app/sub)
tz(build -o ../app.exe ../main.tz IN app/sub STDERR "may overflow int")

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
