// A call in tail position to the function itself, or to another function of
// the same cycle of such calls, is a jump: the stack does not grow, however
// deep the recursion (node would run out of stack here).
function sumTo(n: int, acc: long): long {
  if (n == 0) return acc
  return sumTo(n - 1, acc + n)
}
console.log(sumTo(10000000, 0))

function isEven(n: int): boolean { return n == 0 ? true : isOdd(n - 1) }
function isOdd(n: int): boolean { return n == 0 ? false : isEven(n - 1) }
console.log(isEven(10000000), isOdd(7777777))

// The right side of `&&` and `||` is in tail position too.
function find(a: int[], x: int, i: int): boolean {
  return i < a.length && (a[i] == x || find(a, x, i + 1))
}
const big: int[] = []
for (let i = 0; i < 3000000; i++) big.push(i)
console.log(find(big, 2999999, 0), find(big, -1, 0))

// A call that ends a function that returns nothing.
function countdown(n: int, out: int[]): void {
  if (n == 0) return
  if (n % 1000000 == 0) out.push(n)
  countdown(n - 1, out)
}
const marks: int[] = []
countdown(5000000, marks)
console.log(marks)

// Strings and arrays passed along are released as the calls go.
function joinAll(parts: string[], i: int, acc: string): string {
  return i == parts.length ? acc : joinAll(parts, i + 1, acc + (parts[i] ?? ""))
}
console.log(joinAll(["a", "b", "c"], 0, ""))

// Functions declared in a block, and an arrow that calls itself.
function outer(limit: int): long {
  function loop(i: int, total: long): long {
    return i > limit ? total : loop(i + 1, total + i)
  }
  function ping(n: int): int { return n <= 0 ? 0 : pong(n - 1) }
  function pong(n: int): int { return n <= 0 ? 1 : ping(n - 1) }
  return loop(0, 0) + ping(limit)
}
console.log(outer(4000000))
const countDown = (n: int): int => (n == 0 ? 42 : countDown(n - 1))
console.log(countDown(5000000))

// `??`, optional results and default values.
function opt(n: int): int | undefined {
  return n < 0 ? undefined : n == 0 ? 7 : opt(n - 1)
}
console.log(opt(3000000), opt(-1))
function withDefault(n: int, step = 1): int {
  return n <= 0 ? n : withDefault(n - step)
}
console.log(withDefault(5000000))
function firstDefined(xs: (int | undefined)[], i: int): int | undefined {
  return i >= xs.length ? undefined : xs[i] ?? firstDefined(xs, i + 1)
}
const sparse = new Array<int | undefined>(2000000)
sparse.push(5)
console.log(firstDefined(sparse, 0))
