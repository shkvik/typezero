// same-as-node
// Strings of up to 8 one-byte characters are stored inline, without an
// allocation. Every way of making a string must give the same results as a
// string kept in a buffer, around the 8/9 character boundary in particular.

function eq(a: string, b: string): string {
  return a === b ? "=" : "!"
}

// Built in different ways, compared with each other and with literals.
const parts: string[] = ["", "a", "ab", "abcdefg", "abcdefgh", "abcdefghi", "é", "€", "\u0000"]
for (const p of parts) {
  const built = p.slice(0, 1) + p.slice(1)
  const repeated = p.repeat(2)
  const joined = [p, p].join("")
  console.log(JSON_like(p), p.length, eq(p, built), eq(repeated, joined), eq(repeated, p + p))
}

function JSON_like(s: string): string {
  let out = "["
  for (let i = 0; i < s.length; i++) {
    if (i > 0) out += ","
    out += s.charCodeAt(i)
  }
  return out + "]"
}

// Comparisons with literals, which need no call for short ones.
let word = "abc"
word += "def"
console.log(word === "abcdef", word !== "abcdef", word === "abcdeg", word === "abcdefg")
word += "gh"
console.log(word === "abcdefgh", word.length)
word += "i"
console.log(word === "abcdefghi", word === "abcdefgh", word.length)
let empty = word.slice(3, 3)
console.log(empty === "", "" === empty, empty.length, word.slice(2, 5) === "cde")
let latin = "caf"
latin += "é"
console.log(latin === "café", latin.length, latin.charCodeAt(3))
let euro = "1"
euro += "€"
console.log(euro === "1€", euro.length, euro.charCodeAt(1))
let nul = "\u0000"
console.log(nul === "\u0000", nul === "", nul.length, (nul + nul).length)

// Concatenations that cross the inline limit, and ropes with inline halves.
const letters = "abcdefghijklmnopqrst"
let grow = ""
const sizes: int[] = []
for (let i = 0; i < letters.length; i++) {
  grow += letters[i]
  sizes.push(grow.length)
}
console.log(grow, sizes.join(" "))
const halves: string[] = []
for (let i = 0; i < 6; i++) halves.push("ab".repeat(i) + "|" + "cd".repeat(5 - i))
console.log(halves.join(" "))
const rope = "short" + "a much longer piece of text" + "end"
console.log(rope, rope.length, rope.slice(0, 5) === "short", rope.endsWith("end"))

// Numbers turned into strings, short and long.
const nums: string[] = []
nums.push("" + 7, "" + -1234567, "" + 12345678, "" + 123456789, "" + 1.5, "" + 0.1, "" + 1e21)
nums.push(`${42}`, `n${99999999}`, `${-0.5}x`)
console.log(nums.join(" | "))
console.log(nums[0] === "7", nums[2] === "12345678", nums[3] === "123456789", nums[4] === "1.5")

// Characters taken out of strings.
const text = "héllo wörld, a longer text"
let chars = ""
for (const c of text) if (c !== " ") chars += c
console.log(chars, text[1] === "é", text.at(-1) === "t", text.slice(-4) === "text")

// Searching and sorting short strings.
const names: string[] = ["bob", "al", "zoe", "eve", "alexander", "al", "Bob", "émile"]
console.log(names.indexOf("al"), names.lastIndexOf("al"), names.includes("eve"), names.includes("ev"))
console.log(names.slice().sort().join(","))
console.log("al" < "alexander", "bob" > "Bob", "zoe" <= "zoe", "a\u0000" > "a")
console.log("abc".startsWith("ab"), "abc".endsWith("bc"), "abc".indexOf("c"), "abcabc".includes("ca"))
const eight = "abcdefgh"
console.log(eight.startsWith("cd", 2), eight.startsWith("abcdefgh"), eight.startsWith("h", 7), eight.startsWith("h", 8))
console.log(eight.endsWith("gh"), eight.endsWith("ef", 6), eight.endsWith("a", 1), eight.endsWith("abcdefgh"), eight.endsWith(""))
console.log("abc".startsWith("abcd"), "abc".endsWith("zabc"), "café".endsWith("é"), "é!".startsWith("é"), "x€".endsWith("€"))
