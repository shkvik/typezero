// There is no `as`; numbers convert the way JavaScript converts them.
const d: double = 2.5
const l: long = 5
const i: int = 3
function find(flag: boolean): int | undefined {
  if (flag) return 1
}
const maybe = find(true)
// expect-error: 'as' is not supported; drop the fraction with 'x | 0'
const a = d as int
// expect-error: 'as' is not supported; round with Math.round
const b = d as long
// expect-error: 'as' is not supported; the value converts to 'double' by itself
const c = i as double
// expect-error: 'as' is not supported; Math.trunc(x)
const e = l as int
// expect-error: 'as' is not supported; the value may be undefined
const f = maybe as int
// expect-error: 'as' is not supported; declare the type of the variable instead
const g = [] as int[]
// expect-error: 'as' is not supported, and type 'string' cannot become 'int'
const h = "7" as int
// expect-error: type 'double' is not assignable to type 'int'; drop the fraction with 'x | 0'
const j: int = d
// expect-error: type 'long' is not assignable to type 'int'; Math.trunc(x)
const k: int = l
// expect-error: operator '>>>' cannot be applied to type 'double'
const m = d >>> 0
// expect-error: expected 1 argument, but got 2
const n = Math.floor(d, 2)
// expect-error: property 'sqrtt' does not exist on 'Math'
const p = Math.sqrtt(d)
// expect-error: 'Math.floor' can be used as a value only where a function type is expected
const q = Math.floor
// expect-error: argument of type 'string' is not assignable to parameter of type 'double'
const r = Math.round("1.5")
