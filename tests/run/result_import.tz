// Code written for neverthrow imports it; the names are built in, so the
// import changes nothing but the names it gives. The program's own names
// hide the built-in ones, as any declaration hides an outer one.
import { ok, err as fail, Result as Outcome, Ok } from "neverthrow"

function check(n: int): Outcome<int, string> {
  return n > 0 ? ok(n) : fail(`${n} is not positive`)
}
console.log(check(1), check(0), [1, -1].map(fail))
// err is still there without the import.
console.log(err("built in"))

function known(): Ok<string, string> {
  return ok("always")
}
console.log(known().value)

// A parameter named err, as in neverthrow's own examples of callbacks.
function report(err: string) {
  return `report: ${err}`
}
console.log(report("disk full"), check(-2).mapErr(err => err.length))
function answer(ok: boolean) {
  return ok ? "yes" : "no"
}
console.log(answer(true), answer(false))
