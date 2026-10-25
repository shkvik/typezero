// sort() without a comparison function compares the string forms, as in
// JavaScript, so numbers would come out in another order than their values.
const nums = [2, 10, 1]
// expect-error: 'sort()' without a comparison function compares numbers as strings
// expect-error: pass one: 'sort((x, y) => x - y)'
nums.sort()
const prices = [1.5, 10, 2.25]
// expect-error: compares numbers as strings
console.log(prices.slice().sort())
const gaps: (int | undefined)[] = [3, undefined, 1]
// expect-error: pass one: 'sort((x, y) => (x ?? 0) - (y ?? 0))'
gaps.sort()
// With a comparison function, and for strings and booleans, it is fine.
console.log(nums.sort((x, y) => x - y), ["b", "a"].sort(), [true, false].sort())
