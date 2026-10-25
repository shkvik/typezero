// The turns of the microtask queue each function and method of ResultAsync
// takes are those of neverthrow 8's code, so what runs meanwhile runs in the
// same order: map, mapErr, orElse and the tees are an async function whose
// promise the ResultAsync follows, andThen, match and unwrapOr a callback of
// then(). The expected output is that of neverthrow under node, ticks and
// all.
import { ok, err, okAsync, errAsync, Result, ResultAsync } from "neverthrow"
import { fromPromise, fromSafePromise, fromAsyncThrowable } from "neverthrow"

const log = (s: string) => console.log(s)

// One line per turn of the queue.
let tick = 0
const next = (): Promise<void> => {
  tick++
  log(`t${tick}`)
  return tick < 14 ? Promise.resolve().then(next) : Promise.resolve()
}
void Promise.resolve().then(next)

const bad: ResultAsync<number, string> = errAsync("e")
const seven: ResultAsync<number, string> = okAsync(7)

void okAsync(1).map((x) => x + 1).match((v) => log(`map ${v}`), (e) => log(`map err ${e}`))
void okAsync(1)
  .map(async (x) => x + 2)
  .match((v) => log(`map async ${v}`), (e) => log(`map async err ${e}`))
void bad.map((x) => x + 1).match((v) => log(`map of err ${v}`), (e) => log(`map of err err ${e}`))
void bad.mapErr((e) => `${e}!`).match((v) => log(`mapErr ${v}`), (e) => log(`mapErr err ${e}`))
void bad.mapErr(async (e) => `${e}?`)
  .match((v) => log(`mapErr async ${v}`), (e) => log(`mapErr async err ${e}`))
void okAsync(1)
  .andThen((x) => ok(x * 10))
  .match((v) => log(`andThen ${v}`), (e) => log(`andThen err ${e}`))
void okAsync(1)
  .andThen((x) => okAsync(x * 20))
  .match((v) => log(`andThen async ${v}`), (e) => log(`andThen async err ${e}`))
void seven
  .andThen((x) => err(`no ${x}`))
  .match((v) => log(`andThen to err ${v}`), (e) => log(`andThen to err err ${e}`))
void bad.orElse((e) => ok(e.length)).match((v) => log(`orElse ${v}`), (e) => log(`orElse err ${e}`))
void bad.orElse((e) => okAsync(e.length + 5))
  .match((v) => log(`orElse async ${v}`), (e) => log(`orElse async err ${e}`))
void seven.orElse((e) => ok(e.length))
  .match((v) => log(`orElse of ok ${v}`), (e) => log(`orElse of ok err ${e}`))
void okAsync(3)
  .andTee((x) => log(`andTee saw ${x}`))
  .match((v) => log(`andTee ${v}`), (e) => log(`andTee err ${e}`))
void okAsync(4)
  .andTee(async (x) => log(`andTee async saw ${x}`))
  .match((v) => log(`andTee async ${v}`), (e) => log(`andTee async err ${e}`))
void bad.orTee((e) => log(`orTee saw ${e}`))
  .match((v) => log(`orTee ${v}`), (e) => log(`orTee err ${e}`))
void okAsync(6)
  .andTee(async (x) => {
    await Promise.resolve()
    log(`andTee waited ${x}`)
  })
  .match((v) => log(`andTee waited ${v}`), (e) => log(`andTee waited err ${e}`))
void okAsync(9).unwrapOr(0).then((v) => log(`unwrapOr ${v}`))
void bad.unwrapOr(4).then((v) => log(`unwrapOr of err ${v}`))
void okAsync(1)
  .match((v) => Promise.resolve(v + 1), () => 0.5)
  .then((v) => log(`match promise ${v}`))

void fromPromise(Promise.resolve(5), (e) => e.message)
  .match((v) => log(`fromPromise ${v}`), (e) => log(`fromPromise err ${e}`))
void fromPromise(Promise.reject(new Error("boom")), (e) => `caught ${e.message}`)
  .match((v) => log(`fromPromise of rejected ${v}`), (e) => log(`fromPromise of rejected err ${e}`))
void fromSafePromise(Promise.resolve(6))
  .match((v) => log(`fromSafePromise ${v}`), (e) => log(`fromSafePromise err ${e}`))
void ok(2).asyncMap(async (x) => x + 100)
  .match((v) => log(`asyncMap ${v}`), (e) => log(`asyncMap err ${e}`))
const ex: Result<number, string> = err("x")
void ex.asyncMap(async (x) => x + 100)
  .match((v) => log(`asyncMap of err ${v}`), (e) => log(`asyncMap of err err ${e}`))
void ok(2).asyncAndThen((x) => okAsync(x + 200))
  .match((v) => log(`asyncAndThen ${v}`), (e) => log(`asyncAndThen err ${e}`))

// fromAsyncThrowable: an Err now of what the function throws, or one turn
// after its promise settles; a toError that throws rejects the ResultAsync.
const boom = (x: number): number => {
  if (x > 0)
    throw new Error(`boom ${x}`)
  return x
}
const risky = fromAsyncThrowable(async (x: number) => boom(x), (e) => `caught ${e.message}`)
void risky(3).match((v) => log(`risky ${v}`), (e) => log(`risky err ${e}`))
void risky(0).match((v) => log(`risky ${v}`), (e) => log(`risky err ${e}`))
const now = (x: number): Promise<number> => {
  if (x > 0)
    throw new Error("now")
  return Promise.resolve(x)
}
void fromAsyncThrowable(now, (e) => e.message)(1)
  .match((v) => log(`thrown now ${v}`), (e) => log(`thrown now err ${e}`))
const harsh = fromAsyncThrowable(now, (e) => {
  if (e.message.length > 0)
    throw new Error(`again after ${e.message}`)
  return "never"
})
harsh(1)
  .match((v) => log(`harsh ${v}`), (e) => log(`harsh err ${e}`))
  .catch((e) => log(`harsh rejected ${e.message}`))

// A callback that throws rejects the ResultAsync; so does a rejected
// promise, which goes through every method.
okAsync(1)
  .map((x) => boom(x))
  .match((v) => log(`map throws ${v}`), (e) => log(`map throws err ${e}`))
  .catch((e) => log(`map rejected ${e.message}`))
okAsync(2)
  .andThen((x) => ok(boom(x)))
  .match((v) => log(`andThen throws ${v}`), (e) => log(`x err ${e}`))
  .catch((e) => log(`andThen rejected ${e.message}`))
bad.orElse((e) => ok(boom(e.length)))
  .match((v) => log(`orElse throws ${v}`), (e) => log(`x err ${e}`))
  .catch((e) => log(`orElse rejected ${e.message}`))
fromPromise(Promise.reject(new Error("no")), (e) => `${boom(e.message.length)}`)
  .match((v) => log(`toError throws ${v}`), (e) => log(`toError throws err ${e}`))
  .catch((e) => log(`toError rejected ${e.message}`))
fromSafePromise(Promise.reject(new Error("down")))
  .map((x) => x)
  .andThen((x) => ok(x))
  .match((v) => log(`down ${v}`), (e) => log(`down err ${e}`))
  .catch((e) => log(`down rejected ${e.message}`))
void okAsync(8)
  .andTee((x) => {
    boom(x)
  })
  .match((v) => log(`andTee throws ${v}`), (e) => log(`andTee throws err ${e}`))
void okAsync(7)
  .andTee(async (x) => boom(x))
  .match((v) => log(`andTee rejected ${v}`), (e) => log(`x err ${e}`))

// Halves of other types convert on the way, in the turns following takes.
const two: ResultAsync<number, string> = okAsync(2)
void two
  .andThen((x) => okAsync(x / 4))
  .match((v) => log(`converted ${v}`), (e) => log(`converted err ${e}`))
void bad.orElse((e) => okAsync(e.length * 0.5))
  .match((v) => log(`orElse converted ${v}`), (e) => log(`x err ${e}`))
