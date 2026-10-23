// same-as-node
// What tsc and node accept, and the compiler takes as it is: `export`,
// `readonly` arrays, and integer literals beyond 64 bits.
export function area(w: int, h: int): int {
  return w * h
}
export const UNIT = "m2"
export type Size = int

function pick(a: int, b?: int): int {
  return b ?? a
}

function total(xs: readonly int[]): int {
  let s = 0
  for (const x of xs) s += x
  return s
}
function firstOf(xs: ReadonlyArray<string>): string {
  return xs[0] ?? ""
}
const side: Size = 4
console.log(`${area(side, 3)}${UNIT}`, pick(1), pick(1, 2), total([1, 2, 3]), firstOf(["a"]))

// Too large for 64 bits: a double, as any number in JavaScript.
console.log(123456789012345678901234567890, 0xfffffffffffffffffff, 18446744073709551615, -9223372036854775809)
