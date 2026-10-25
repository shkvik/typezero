// same-as-node
// Regex literals, test, exec and match: groups, index, named groups.
const date = /(?<year>\d{4})-(?<month>\d{2})(-\d{2})?/
console.log(date.test("on 2024-01"), date.test("no date"), /^\d+$/.test("123"), /^\d+$/.test("12a"))

const m = date.exec("on 2024-01, then")
console.log(m)
if (m) {
  const year: string = m[1]
  const day: string | undefined = m[3]
  console.log(year, m[2], day, m.index, m.input, m.length)
  console.log(m.groups.year, m.groups.month)
  const { year: y, month } = m.groups
  console.log(y, month)
}
console.log(date.exec("nothing here") == undefined)

// match without g: the first match; with g: the texts.
const first = "a1b22c333".match(/\d+/)
console.log(first?.[0], first?.index)
console.log("a1b22c333".match(/\d+/g), "abc".match(/\d+/g) == undefined)
console.log("Hello World".match(/o/g)?.length)

// Destructuring a match, and its captures as an array.
const parts = "key=value".match(/(\w+)=(\w+)/)
if (parts) {
  const [all, key, value] = parts
  console.log(all, key, value)
  console.log(parts.slice(1), parts.map(p => p ?? "-").join("|"))
  for (const p of parts) console.log(p)
  console.log([...parts].length)
}

// Optional groups are undefined when they take no part.
const opt = /(a)|(b)/.exec("b")
if (opt) console.log(opt[1], opt[2], opt[1] ?? "none")
console.log(/(?:(a)|b)+/.exec("ab"))

// lastIndex: with g, exec goes on from the last match.
const word = /\w+/g
let found: RegExpExecArray | undefined
while ((found = word.exec("one two three")) != undefined) console.log(found[0], found.index, word.lastIndex)
console.log(word.lastIndex)
const sticky = /foo/y
sticky.lastIndex = 3
console.log(sticky.test("barfoo"), sticky.lastIndex, sticky.test("barfoo"), sticky.lastIndex)

// Each evaluation of a literal with g is a new regex.
function count(text: string): int {
  const re = /x/g
  let n = 0
  while (re.exec(text) != undefined) n++
  return n
}
console.log(count("xaxbx"), count("xx"))

// Properties and printing.
const flags = /a/gimsuy
console.log(flags.source, flags.flags, flags.global, flags.ignoreCase, flags.multiline)
console.log(flags.dotAll, flags.unicode, flags.sticky, flags.hasIndices, flags.unicodeSets)
console.log(/[/]\//.source, new RegExp("a/b").source, new RegExp("").source, String(/a\n/))
console.log(`${/x/g}`, /x/g.toString(), [/a/, /b/i], { re: /c/ })
console.log(/x/ == /x/, typeof /x/)

// Named groups that may take no part.
const named = /(?<word>[a-z]+)|(?<number>\d+)/.exec("42")
console.log(named)
if (named) console.log(named.groups.word ?? "-", named.groups.number)

// `/` divides where an expression ends, and starts a regex where one starts.
const half = 10 / 2 / 5
console.log(half, [4].map(x => x / 2)[0], /2/.test("2") ? 6 / 3 : 0, 9 /3/ 1)

// A regex method runs no code of the program, so what is known of indices stays.
const lines = ["a1", "b", "c3"]
for (let i = 0; i < lines.length; i++) if (/\d/.test(lines[i])) console.log(lines[i].toUpperCase())

// `?? []`: a match on the left is the array of its captures.
const [, key, value] = "k=v".match(/(\w)=(\w)/) ?? []
const [, none] = "--".match(/(\w)=(\w)/) ?? []
console.log(key, value, none, ("abc".match(/x/g) ?? []).length)
