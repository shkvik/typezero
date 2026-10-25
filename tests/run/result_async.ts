// ResultAsync of neverthrow: a Promise of a Result, with neverthrow's
// functions and methods. The expected output is that of neverthrow 8 under
// node (the test does not run under node, which would need the package).
import { okAsync, errAsync, ResultAsync, ok, err, Result } from "neverthrow"
async function fetchAge(name: string): Promise<int> {
  await undefined
  if (name == "") throw new Error("no name")
  return name.length * 10
}
function age(name: string): ResultAsync<int, string> {
  return ResultAsync.fromPromise(fetchAge(name), e => `failed: ${e.message}`)
}
console.log(await age("Ann"))
console.log(await age(""))
const doubled = await age("Bo").map(a => a * 2)
console.log(doubled)
const asyncMapped = await age("Bo").map(async a => {
  await undefined
  return `${a} years`
})
console.log(asyncMapped)
console.log(await age("").mapErr(e => e.length))
const chained = await age("Cid").andThen(a => a > 20 ? ok(a) : err("too young"))
console.log(chained)
const chainedAsync = await age("Di").andThen(a => okAsync(a + 1))
console.log(chainedAsync)
console.log(await age("").orElse(e => ok(0)))
console.log(await age("Eve").match(a => `age ${a}`, e => `error ${e}`))
console.log(await age("").match(a => `age ${a}`, e => `error ${e}`))
console.log(await age("").unwrapOr(-1), await age("Fay").unwrapOr(-1))
const teed = await age("Gus").andTee(a => console.log("tee saw", a))
console.log(teed)
console.log(await okAsync(5), await errAsync("bad"))
const safe = ResultAsync.fromSafePromise(fetchAge("Hal"))
console.log(await safe)
const parse = ResultAsync.fromThrowable(fetchAge, e => e.message)
console.log(await parse("Ivy"), await parse(""))
function check(n: int): Result<int, string> {
  return n > 0 ? ok(n) : err("not positive")
}
console.log(await check(3).asyncMap(async n => n * 3))
console.log(await check(-1).asyncAndThen(n => okAsync(n)))
