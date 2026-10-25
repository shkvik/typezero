// Lists of the types of parameters: what generic code can do with them, and
// lists that a call cannot work out.
function wrap<A extends unknown[], B extends unknown[], R>(fn: (...args: A) => R, other: (...args: B) => R) {
  return (...args: A): R => {
    // expect-error: the arguments for the parameters 'A' of some function are spread from a list of them, e.g. '...args'
    fn(1)
    // expect-error: '...A' is spread where the parameters 'B' take '...B'
    other(...args)
    return fn(...args)
  }
}
function both<A extends unknown[], B extends unknown[]>(fn: (...args: [...A, ...B]) => void) { return 1 }
function three(a: int, b: int, c: int) {}
// expect-error: cannot infer type parameter 'A' of 'both': nothing in the call says what it is; give it, e.g. 'both<[int], [int]>(...)'
both(three)
// expect-error: 'A' is a list of types; spread it into the tuple, '...A'
function notSpread<A extends unknown[]>(fn: (...args: [A]) => void) { return 1 }
// expect-error: a rest element in a tuple type is not supported: a tuple has a fixed length; use an array, e.g. 'int[]'
let open: [string, ...int[]] = ["a"]
// expect-error: only an array can be spread into console.log, found 'string'
console.log(..."text")
// expect-error: nothing can follow the array spread into a list: it takes the arguments to the end
function after(...args: [...int[], string]) { return 1 }
