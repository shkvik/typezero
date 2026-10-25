// same-as-node
// matchAll, the syntax of patterns, new RegExp and RegExp.escape.
const text = "a=1, bb=22, ccc=333"
for (const { groups } of text.matchAll(/(?<key>\w+)=(?<value>\w+)/g))
  console.log(groups.key, groups.value)
for (const m of text.matchAll(/(\w+)=(\d+)/g))
  console.log(m.index, m[1], m[2].length)
for (const [all, key] of text.matchAll(/(\w+)=/g)) console.log(all, key)
console.log([...text.matchAll(/\d+/g)].map(m => m[0]), [..."aaa".matchAll(/(?:)/g)].length)
const g = /b/g
g.lastIndex = 5
console.log([..."abcabc".matchAll(g)].map(m => m.index), g.lastIndex)
// A lazy iterator: each match is found when it is asked for.
const it = text.matchAll(/\d+/g)
console.log(it.next().value?.[0], it.next().value?.index, it)
console.log(text.matchAll(/(\w+)=/g).map(m => m[1].toUpperCase()).toArray())
console.log(Array.from("x1y22".matchAll(/\d+/g), m => m[0].length), it.next().done)

// The syntax: lookahead and lookbehind, references, lazy loops, classes.
console.log("price: $42, cost: $7".match(/(?<=\$)\d+/g), "$4 €5".match(/(?<!\$)\d/g))
console.log(/(\w)\1/.exec("abccd")?.[0], /(?<c>\w)\k<c>/.exec("xyyz")?.[0])
console.log("<a><b>".match(/<.+?>/)?.[0], "<a><b>".match(/<.+>/)?.[0], "aaa".match(/a{2,}?/)?.[0])
console.log("x-y_z".match(/[\w-]+/)?.[0], "a]b".match(/[\]]/)?.index, "tab\there".split(/\s/))
console.log(/^\p{Lu}\p{Ll}+$/u.test("Ðúnn"), /\p{Script=Cyrillic}+/u.exec("hi привет")?.[0])
console.log(/[\p{L}--[a-z]]+/v.exec("abcDEF")?.[0], /[\q{abc|d}]/v.exec("xabc")?.[0])
console.log(/(?i:a)b/.test("Ab"), /(?i:a)b/.test("AB"), /a(?-i:b)/i.test("Ab"), /a(?-i:b)/i.test("AB"))
console.log(/straße/i.test("STRASSE"), /ß/iu.test("ẞ"), /K/i.test("k"), /K/iu.test("k"))
console.log(/^.$/.test("😀"), /^.$/u.test("😀"), "😀".match(/./g)?.length, "😀".match(/./gu)?.length)
console.log(/a.c/s.test("a\nc"), /a.c/.test("a\nc"), /^b$/m.test("a\nb\nc"), /^b$/.test("a\nb\nc"))
console.log(/\bcat\b/.test("a cat!"), /\Bcat/.test("concat"), /(?:)/.exec("x")?.index)
console.log(/(a)|(b)/d.exec("b")?.indices, /x/y.exec("ax") == undefined)

// A pattern known only when the program runs.
const separator = "."
const parts = "a.b.c".split(new RegExp(RegExp.escape(separator)))
console.log(parts, RegExp.escape("a.b*c"), RegExp.escape("x y"), RegExp.escape("0"))
const word = "c[a]t"
const re = new RegExp(RegExp.escape(word), "gi")
console.log("C[A]T c[a]t".replace(re, "dog"), re.source, re.flags, re.global)
const pattern = ["(\\d+)", "(\\d+)"].join("-")
const dynamic = new RegExp(pattern)
const m = dynamic.exec("10-20")
if (m) console.log(m[1], m[2], m[3], m.length)
console.log(RegExp("^a+$").test("aaa"), new RegExp("x", "g").flags, new RegExp(`a${1 + 1}`).source)
const name: string = "k"
console.log(new RegExp(name + "+", "i").exec("xKkk")?.[0])
