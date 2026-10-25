// same-as-node
// replace, replaceAll, split and search with regexes.
const date = "2024-01-15 and 2023-12-31"
console.log(date.replace(/(\d{4})-(\d{2})-(\d{2})/, "$3.$2.$1"))
console.log(date.replace(/(\d{4})-(\d{2})-(\d{2})/g, "$3.$2.$1"))
console.log(date.replaceAll(/(?<y>\d{4})-(?<m>\d{2})-(?<d>\d{2})/g, "$<d>/$<m>/$<y>"))
console.log("a-b-c".replace(/-/g, "+"), "a-b-c".replace(/-/, "+"), "abc".replace(/x/, "y"))
console.log("price".replace(/r/, "[$&|$`|$'|$$]"), "aaa".replace(/a/g, "$$"))
console.log("x".replace(/(x)/, "$1$0$10$01"), "ab".replace(/(a)(b)/, "$2$1$12"))
console.log("abc".replace(/(?:)/g, "-"), "😀😀".replace(/(?:)/gu, "-"), "aaa".replace(/a*?/g, "<$&>"))
console.log("Hello World".replace(/o/gi, "0"), "line1\nline2".replace(/^/gm, "> "))

// With a function: the match, the groups, the offset and the string.
console.log("2024-01-15".replace(/(\d+)-(\d+)-(\d+)/, (all, y, m, d) => `${d}.${m}.${y}`))
console.log("a1b22c333".replace(/\d+/g, n => `[${n.length}]`))
console.log("one two".replace(/\w+/g, (word, offset: int) => `${word}@${offset}`))
console.log("x-y".replace(/(\w)-(\w)/, (all, a, b, offset, text) => `${b}-${a} in ${text} at ${offset}`))
console.log("ab".replace(/(a)|(b)/g, (all, a, b) => a ?? `(${b ?? "?"})`))
console.log("k=v;n=1".replace(/(?<key>\w+)=(?<value>\w+)/g, (all, key, value, offset, text, groups) =>
  `${groups.value}=${groups.key}`))
console.log("aaa".replaceAll(/a/g, (a, i: int) => String(i)))
function shout(text: string) { return text.toUpperCase() }
console.log("hey you".replace(/\b\w/g, shout))
let calls = 0
console.log("abc".replace(/./g, c => { calls++; return c + c }), calls)

// split: the pieces between the matches, with the groups between them.
console.log("a1b2c3".split(/\d/), "a1b2c3".split(/(\d)/), "a, b,c".split(/\s*,\s*/))
console.log("abc".split(/(?:)/), "".split(/x/), "".split(/(?:)/), "abc".split(/b/, 1), "abc".split(/(b)/, 2))
console.log("a1b".split(/(x)?1/), "test".split(/(?=s)/), "😀a😀".split(/(?:)/u))
const pieces: (string | undefined)[] = "a1b".split(/(x)?1/)
console.log(pieces.length, pieces[1] ?? "undefined group")

// search: where the first match is, ignoring lastIndex.
const g = /o/g
g.lastIndex = 5
console.log("hello world".search(/o/), "hello".search(/z/), "hello world".search(g), g.lastIndex)
console.log("abc".search(/(?:)/), "Hello".search(/l+/))
