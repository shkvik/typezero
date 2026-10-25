// same-as-node
// Regex literals run as machine code (codegen_regex_native.cpp): the scans
// for where a match starts, in strings of bytes and of two-byte units, the
// shortcuts of alternatives and repeats, batches of matches and the memo
// against blowups give the same results as JavaScript.

// Text long enough for the scans that read 32 and 16 bytes at a time, with
// matches near the start, in the middle and at the very end.
const words = ["running", "data", "error", "the", "warning", "fatal", "agggtaaa", "TTTACCCT",
  "thing", "2024-01-15", "warningX", "x1"]
const parts: string[] = []
for (let i = 0; i < 300; i++) parts.push(words[(i * 7) % words.length] ?? "")
const text = parts.join(" ")
const wide = `${text} Σ — ${text}` // two bytes a unit

for (const s of [text, wide]) {
  console.log(s.length, s.match(/[a-z]+ing/g)?.length, s.match(/agggtaaa|tttaccct/gi)?.length,
    s.match(/\b(?:error|warning|fatal)\b/g)?.length, s.search(/\d{4}-\d{2}/),
    s.match(/(\d{4})-(\d{2})-(\d{2})/g)?.length, s.replace(/(\d{4})-(\d{2})-(\d{2})/g, "$3.$2.$1").length)
  let n = 0
  let last = ""
  for (const m of s.matchAll(/(\w+)ing (\w+)/g)) {
    n += m[2].length
    last = `${m.index} ${m[1]} ${m[2]}`
  }
  console.log(n, last)
}
for (const end of ["error", "agggtaaa", "2024-01-15", " Σ error"]) {
  const s = "x".repeat(61) + end
  console.log(s.search(/\berror\b/), s.search(/agggtaaa/i), s.search(/\d{4}-\d\d-\d\d/),
    s.match(/r+o/g)?.join() ?? "none")
}

// Alternatives that start alike, and repeats followed by a character.
console.log(/ab|ac|a/.exec("xacab"), /(?:foo|foobar)bar/.exec("foobarbar"))
console.log("a-b ab-c abc".match(/[a-z]+-[a-z]+|ab[a-z]?/g))
console.log(/(\w+)ing/.exec("singing ringing"), /a+b/.exec("caaab aab"), /x*y/.exec("xxzxy"))
console.log("<a><bb><ccc>".match(/<.+?>/g), /a*?b/.exec("aaab"), /(a+?)(a*)/.exec("aaa"))
console.log("1,22,,333".split(/,/), "a1b22c".split(/(\d+)/), "abc".split(/(?:)/), "a b".split(/ /, 1))

// Groups reset on each iteration, references back, lookarounds.
console.log(/(?:(a)|b)+/.exec("ab"), /(a|(b))+/.exec("abab"), /((a)|(b))*/.exec("aab"))
console.log("aa bb cd ee".match(/(\w)\1/g), `say "hi" and 'yo'`.match(/(?<q>['"]).*?\k<q>/g))
console.log(/(a)\1/i.exec("aA"), /\b(\w+) \1\b/.exec("it is is so"))
console.log("$10 $20 30".match(/(?<=\$)\d+/g), "10px 20 30em".match(/\d+(?!px|\d)/g))
console.log(/(?<=(\d+)(\d+))$/.exec("1053"), /(?=(\w+))\w/.exec("abc"), /(?!a)\w+/.exec("aab"))

// Sticky, and lastIndex.
const sticky = /\d/y
sticky.lastIndex = 2
console.log(sticky.exec("ab12"), sticky.lastIndex, sticky.exec("ab12"), sticky.lastIndex)
console.log("12a34".match(/\d/gy), "a12".match(/\d/gy) == undefined)
const g = /o/g
g.lastIndex = 5
console.log(g.exec("foo boo"), g.lastIndex)

// Many matches: several batches of the matcher.
const many = "ab,".repeat(1000)
const all = many.match(/b/g)
console.log(all?.length, all?.[999], many.replace(/a/g, "<$&>").length,
  many.replace(/(a)(b)/g, (m, x, y) => `${y}${x}`).slice(0, 12), many.split(/,/).length)
console.log("abc".match(/x*/g), "abc".replace(/x*/g, "-"), "aaa".match(/a*?/g), "aaa".replace(/a??/g, "."))

// Empty matches with u step over whole surrogate pairs.
const emoji = "😀a😀"
console.log(emoji.match(/(?:)/gu)?.length, emoji.match(/(?:)/g)?.length, emoji.split(/(?:)/u).length,
  emoji.replace(/(?:)/gu, "-").length, emoji.replace(/(?:)/g, "-").length, emoji.match(/./gu))
console.log("x😀y".match(/\p{L}/gu), /^.$/u.test("😀"), /^.$/.test("😀"), "😀😀".match(/😀+/u)?.[0].length)
// A reference back never ends inside a pair, as in V8: here the lookbehind
// fails there, so the empty match is between the halves.
console.log(/(x)|((?<!\1))/v.exec("ab😀c")?.index, /(x)|((?<!\1))/.exec("ab😀c")?.index)
// Inside the group it refers to, a reference is nothing at all (as in V8).
console.log(/(?!(?!(?<y>(?!\k<y>))))/u.exec("a😀b")?.index, /(?!(?!(?!\1)))(x)?/u.exec("a😀b")?.index)

// Patterns that make backtracking blow up, on inputs small enough for
// JavaScript: the memo of the matcher gives the same answers.
const run = "a".repeat(20)
console.log(/^(a+)+$/.test(`${run}!`), /(a|aa)+c/.test(run), /(a*)*b/.exec(run) == undefined,
  /(?:a|a)*$/.exec(`${run}!`))
console.log(/((a)|b)*c/.exec(`${"ab".repeat(9)}c`), /(\w+\s?)+$/.test(`${"word ".repeat(5)}!`))
console.log(/(?=(a+)+$)a/.test(`${run}!`), /(a+?)+b/.exec(`${run}b`)?.[1], /(a{1,3})+b/.exec(`${run}b`)?.[1])
