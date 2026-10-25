// env: TZ=UTC
// Dates with the rest of the language, across files: fields of classes up a
// chain and of interfaces, generic code, Result, generators and iterator
// helpers, sorting with a comparison, join(), JSON.stringify, and the
// errors dates throw.
import { Dated, Event, LateError, Meeting, checkDue, parseWhen } from "./events.ts"
import { err, ok, Result } from "neverthrow"

const base = Date.UTC(2026, 9, 26, 9, 0)
const events: Event[] = [
  new Meeting("standup", new Date(base), 15),
  new Event("release", new Date(base + 3 * 86400000)),
  new Meeting("review", new Date(base - 86400000), 60),
]

// 1. Fields up a chain, through the base class and an interface.
for (const e of events) console.log(e.describe())
const dated: Dated[] = [...events]
console.log(dated.map((d) => d.at.getUTCDay()).join())
console.log(events.map((e) => e.later(7).describe()).join("\n"))

// 2. Sorted with a comparison of the times; join() writes toString().
const sorted = events.slice().sort((a, b) => a.at.getTime() - b.at.getTime())
console.log(sorted.map((e) => e.title).join(" < "))
const moments = sorted.map((e) => e.at)
moments.sort((a, b) => b.getTime() - a.getTime())
// (The name of the zone is in the language of the system: cut off here.)
console.log(moments.join("|").split("|").map((t) => t.slice(0, 33)).join(" | "))
console.log(moments.map((d) => d.toISOString().slice(0, 10)).join())

// 3. Generic code: dates as T.
function latest<T>(items: T[], when: (x: T) => Date): T | undefined {
  let best: T | undefined = undefined
  for (const x of items) if (best == undefined || when(x) > when(best)) best = x
  return best
}
function pair<A, B>(a: A, b: B): [A, B] {
  return [a, b]
}
console.log(latest(events, (e) => e.at)?.title, latest(moments, (d) => d)?.toISOString())
const [first, second] = pair(new Date(base), "after")
console.log(first.getUTCHours(), second)

// 4. Result: a date, or the error that says why there is none.
function due(text: string): Result<Date, string> {
  try {
    return ok(checkDue(parseWhen(text), new Date(base)))
  } catch (e) {
    if (e instanceof LateError) return err(`${e.name}: ${e.message}, due ${e.due.toISOString()}`)
    return err(`${e.name}: ${e.message}`)
  }
}
for (const text of ["2026-10-27T00:00Z", "2026-10-25T21:00Z", "someday"]) {
  const r = due(text)
  console.log(r.isOk() ? `ok ${r.value.toISOString()}` : `err ${r.error}`)
}

// 5. Generators and iterator helpers give dates, and close with finally.
const log: string[] = []
function* days(from: Date, count: int): Generator<Date> {
  try {
    // A day in milliseconds is a long: 25 of them no longer fit in an int.
    for (let i: long = 0; i < count; i++) yield new Date(from.getTime() + i * 86400000)
  } finally {
    log.push("days done")
  }
}
for (const d of days(new Date(base), 30)) {
  if (d.getUTCDay() == 0) {
    console.log("first sunday", d.toISOString().slice(0, 10))
    break
  }
}
const weekdays = days(new Date(base), 7)
  .filter((d) => d.getUTCDay() != 0 && d.getUTCDay() != 6)
  .map((d) => d.toLocaleDateString("en-US", { weekday: "short", timeZone: "UTC" }))
  .toArray()
console.log(weekdays.join(), log.join(", "))

// 6. JSON: a date is its ISO text, an invalid one null; in an object, an
// array, a class and a subclass.
console.log(JSON.stringify(events[0]), JSON.stringify(events[1]))
console.log(JSON.stringify({ when: new Date(base), never: new Date(NaN), list: moments.slice(0, 1) }))
const saved = JSON.stringify({ title: "x", at: new Date(base) })
const back = JSON.parse<{ title: string; at: string }>(saved)
console.log(back.title, new Date(back.at).getTime() == base)

// 7. What dates throw, caught.
const bad = parseWhenLoosely("nope")
function parseWhenLoosely(text: string): Date {
  return new Date(text)
}
try {
  console.log(bad.toISOString())
} catch (e) {
  console.log(e instanceof RangeError, e.message, bad.toJSON() ?? "null", `${bad}`)
}
try {
  checkDue(new Date(base), new Date(base + 5400000))
} catch (e) {
  if (e instanceof LateError) console.log(e.name, e.message, e.now.getTime() - e.due.getTime())
}
const zones = ["Asia/Tokyo", "Nowhere/Land"]
for (const timeZone of zones) {
  try {
    console.log(new Date(base).toLocaleTimeString("en-US", { timeZone, hour: "numeric" }))
  } catch (e) {
    console.log(`${e}`)
  }
}
console.log(new Meeting("printed", new Date(base), 5))
