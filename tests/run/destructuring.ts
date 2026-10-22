// same-as-node
// Destructuring arrays: in declarations, assignments and for...of.
const [a, b] = [1, 2]
console.log(a, b)

let x = 1
let y = 2
;[x, y] = [y, x]
console.log(x, y)

// Past the end of an array JavaScript gives undefined, so an element not
// proven to exist has type `T | undefined`.
const [year, month, day, extra] = "2026-10-22".split("-")
console.log(year, month, day, extra)
const date = "2026-10-22".split("-")
if (date.length >= 3) {
  const [yy, mm, dd] = date // proven: plain strings
  console.log(yy.length + mm.length + dd.length)
}

// Defaults, holes, the rest and nested patterns.
const [p = "?", q = "?", r = "none"] = ["x", "y"]
console.log(p, q, r)
const [first, , third, ...others] = [10, 20, 30, 40, 50]
console.log(first, third, others)
const [[n1, n2 = 0], [n3]] = [[1], [3]]
console.log(n1, n2, n3)

// A string destructures into its characters.
const [c1, c2, ...restChars] = "h😀llo"
console.log(c1, c2, restChars)

for (const [name, score = "0"] of [["ann", "3"], ["bob"]]) console.log(name, score)

// Assignment to any target: here two elements swap.
const arr = [1, 2, 3]
;[arr[0], arr[2]] = [arr[2], arr[0]]
console.log(arr)
let lo = 0
let hi = 0
const range = [5, 9]
;[lo = -1, hi = -1] = range
console.log(lo, hi)
let head = ""
let tail: string[] = []
;[head = "", ...tail] = ["a", "b", "c"]
console.log(head, tail)
