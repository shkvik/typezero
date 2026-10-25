// ResultAsync used wrongly: forgotten, given functions of the wrong kind,
// and the parts of neverthrow tz does not have.
import { ok, err, okAsync, Result, ResultAsync, fromPromise } from "neverthrow"

function check(n: number): Result<number, string> {
  return n > 0 ? ok(n) : err("not positive")
}

// A ResultAsync is a Promise: one nobody waits for is an error.
// expect-error: the Promise is not awaited
okAsync(1).map((x) => x + 1)

// Functions that give the wrong thing.
// expect-error: the function of 'andThen' must give a Result or a ResultAsync, found 'int'; to give a value, use 'map'
const a = okAsync(1).andThen((x) => x + 1)
// expect-error: the function of 'asyncMap' must give a Promise, found 'double'; use 'map' for a function that gives a value
const b = check(1).asyncMap((x) => x + 1)
// expect-error: the function of 'asyncAndThen' must give a ResultAsync, found 'Result<double, never>'; use 'andThen' for a function that gives a Result
const c = check(1).asyncAndThen((x) => ok(x))
// expect-error: the function of 'asyncAndThen' must give a ResultAsync, found 'Promise<double>'; to give a value, use 'asyncMap'
const d = check(1).asyncAndThen(async (x) => x)
// expect-error: 'fromPromise' takes a Promise, found 'int'
const e = fromPromise(1, (e) => e.message)
// expect-error: the function that turns the Error into the error of the Result must return it
const f = fromPromise(Promise.resolve(1), (e) => console.log(e.message))
// expect-error: 'fromThrowable' of ResultAsync takes a function that gives a Promise, found '(double) => double'
const g = ResultAsync.fromThrowable((x: number) => x * 2)

// What tz does not have.
// expect-error: 'ResultAsync.combine' is not supported; ResultAsync has 'fromPromise', 'fromSafePromise' and 'fromThrowable'; await them with 'Promise.all' and use 'Result.combine'
const h = ResultAsync.combine([okAsync(1), okAsync(2)])
// expect-error: 'andThrough' of a ResultAsync is not supported yet
const i = okAsync(1).andThrough((x) => ok(x))
