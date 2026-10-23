// same-as-node
// Arrays: literals, indexing, length and the built-in methods, as in JavaScript.
const nums = [1, 2, 3]
console.log(nums, nums.length, nums[0], nums[2])
nums.push(4)
console.log(nums)
let sum = 0
for (let i = 0; i < nums.length; i++) sum += nums[i]
console.log(sum)
for (const x of nums) console.log(x * 10)
const letters = ["a", "b", "c"]
console.log(letters, letters.join("-"), String(nums), `${nums}`, "nums = " + nums)
const none: int[] = []
console.log(none, none.length, none.pop(), none.shift())
console.log([1.5, -0, 2], [true, false], [[1, 2], [3]], ["it's", 'say "hi"'])

// Methods
const a = [3, 1, 4, 1, 5, 9, 2, 6]
console.log(a.indexOf(1), a.lastIndexOf(1), a.includes(9), a.includes(7), a.indexOf(1, 2), a.lastIndexOf(1, 2))
console.log(a.slice(2), a.slice(-3), a.slice(1, 3), a.slice(5, 2), a.at(0), a.at(-1), a.at(100))
console.log(a.concat([7, 8], 9), a.join(), a.join(" | "), a.toString())
// reverse() and sort() change the array and return it; console.log shows the
// state after all its arguments ran. Without a comparison function, sort()
// compares the string forms, as in JavaScript; numbers need one (see
// errors/sort.ts), here one that compares them as strings.
const b = a.slice()
console.log(b.reverse(), b.sort((x, y) => x - y), [10, 9, 1, 100, 25].sort((x, y) => (String(x) < String(y) ? -1 : 1)))
console.log(["b", "a", "C", "ä"].sort(), [true, false, true].sort())
const q: int[] = []
for (let i = 0; i < 5; i++) q.push(i)
console.log(q.shift(), q.shift(), q, q.unshift(-1, -2), q, q.length)
const f = new Array<double>(4).fill(0.5)
console.log(f, f.fill(2, 1, 3), [1, 2, 3, 4, 5].fill(0, -2))
let r = [1, 2, 3]
r.length = 1
console.log(r)
// Spreads, and arrays with undefined elements.
const words = ["alpha", "beta", "gamma"]
const all = [...words, "delta", ...words.slice(1)]
console.log(all, all.length)
const opt: (int | undefined)[] = [1, undefined, 3]
console.log(opt, opt.join("-"), String(opt), opt.includes(undefined), opt.indexOf(3))
// indexOf uses ==, where NaN equals nothing; includes finds NaN.
function notANumber(): double {
  return 0.0 / 0.0
}
const nan = [notANumber(), 1]
console.log(nan.indexOf(notANumber()), nan.includes(notANumber()))
const empty: string[] = []
console.log(empty.pop(), empty.shift(), empty.join(), empty)
const m = [[1, 2], [3, 4]]
console.log(m.join(";"), `${m}`, m.concat([[5]]))
