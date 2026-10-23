function f(flag: boolean): int | undefined {
  if (flag) return 1
}
// expect-error: cannot infer a type from 'undefined'
let u = undefined
let s: string | undefined = "a"
s = undefined
// expect-error: the value may be undefined; check it before reading 'length'
console.log(s.length)
const n = f(false)
// expect-error: operator '+' cannot be applied to types 'int | undefined' and 'int'
const m = n + 1
// expect-error: type 'int | undefined' is not assignable to type 'int'
const k: int = n
// expect-error: 'as' is not supported; the value may be undefined
const c = n as int
// expect-error: the type 'undefined' alone is not supported
let only: undefined
// expect-error: operator '*' cannot be applied to types 'int | undefined' and 'int'
const r = f(true) * 2
let total = 0
// expect-error: '??=' needs a variable that may be undefined
total ??= 1
// After a check, the variable holds a value until it is assigned again.
if (n != undefined) {
  const ok: int = n
}
