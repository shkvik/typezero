// same-as-node
// The fast paths of the string methods must give what the general ones give:
// at the lengths where strings change form (inline up to 8 one-byte units,
// copies up to 12, slices from 13), around the blocks of 16 bytes that the
// searches read, for one-byte and two-byte text, ropes and slices.
function show(parts: string[]): string {
  return parts.map((p) => `${p.length}:${p}`).join("|")
}

// split() and join()
for (let len = 0; len <= 40; len++) {
  for (const every of [1, 2, 5, 9, 13, 17]) {
    let text = ""
    for (let i = 0; i < len; i++) text += i % every == every - 1 ? " " : String.fromCharCode(97 + (i % 26))
    const parts = text.split(" ")
    if (len % 4 == 0 || every == 9) console.log(len, every, show(parts))
    if (parts.join(" ") != text || text.split(" ", 3).length != Math.min(parts.length, 3)) {
      console.log("bad", len, every)
    }
  }
}
const latin = "café crème brûlée naïveté-naïveté éééééééééééééé x"
console.log(show(latin.split(" ")), show(latin.split("é")), latin.split("").length)
const cyr = "привет мир, как дела, всё хорошо, спасибо большое"
console.log(show(cyr.split(" ")), show(cyr.split(", ")), show(cyr.split("о")), cyr.split("").length)
console.log(show("abc".split("я")), show("яяя".split("я")), show("a😀b😀".split("😀")))
console.log("a😀b".split("").map((c) => c.charCodeAt(0)))
const words = "one two three four five six seven eight nine ten"
for (const limit of [0, 1, 2, 9, 10, 11, -1]) {
  console.log(limit, show(words.split(" ", limit)), show(words.split("e", limit)))
  console.log(show(words.split("", limit)))
}
console.log(show("aaa".split("aa")), show("abcabcab".split("ab")), show("xxxyxxxy".split("xxy")))
console.log(show("ab".split("abc")), show("ab".split("ab")))
const longSep = "<separator of more than sixteen units>"
console.log(show(`a${longSep}b${longSep}${longSep}c`.split(longSep)))
const padded = "xxxxx" + "a b c d e f g h i j k l m n o p q r s t u v w x y z alpha beta gamma"
console.log(show(padded.slice(5).split(" ")), show(padded.slice(5, 30).split(" ", 4)))
let rope = "first part of the rope, "
rope = rope + "second part of the rope, " + "third"
console.log(show(rope.split(", ")), show(rope.split(" ")))
const wide = ("я" + "abcdefghijklmnopqrstuvwxyz").slice(1)
console.log(show(`12 abcdefghijklmnopqrstuvwxyz 34 abcdefghijklmnopqrstuvwxyz`.split(wide)))
console.log(show(`${wide} ${wide}`.split(" ")), show(`${wide}-${wide}`.split("-")))
const items = ["short", "a bit longer", "much longer than thirteen", "", "é", "ü long latin text", "я"]
console.log(items.join(), items.join(" | "), items.join(""), items.slice(0, 4).join("--"))
console.log(["x", "", "y"].join(""), ["", ""].join(","), ["exactly8"].join("-"))
console.log(["ab", "cd"].join("1234") == "ab1234cd")

// indexOf(), includes() and lastIndexOf() at every position of a needle
const hay = "the quick brown fox jumps over the lazy dog; the quick brown fox again"
const needles = ["t", "the", "fox", "quick brown fox", "again", "dog;", "lazy dog; the quick", "x", "zz", ""]
for (const needle of needles) {
  const found: int[] = []
  for (let from = 0; from <= hay.length; from += 7) found.push(hay.indexOf(needle, from))
  console.log(needle, found.join(","), hay.lastIndexOf(needle), hay.lastIndexOf(needle, 40))
  console.log(hay.includes(needle, 50))
}
const cyrHay = `${cyr} quick brown fox`
console.log(cyrHay.indexOf("fox"), cyrHay.indexOf("как"), cyrHay.lastIndexOf("о"))
console.log(latin.indexOf("ééé"), latin.indexOf("я"), "abc".indexOf("яbc"))
console.log(`zz ${wide}`.indexOf("klm"), "zz abcdefghijklmnopqrstuvwxyz".indexOf(wide))
console.log("zzabcdefghijklmnopqrstuvwxy".indexOf(wide))

// replaceAll()
console.log("a.b.c".replaceAll(".", "/"), "abc".replaceAll("", "-"), "aaa".replaceAll("aa", "b"))
console.log("x".replaceAll("x", "$&$&"), "abc".replaceAll("b", "") == "ac", "aaaa".replaceAll("a", "bb"))
console.log("hello world".replaceAll("o", "0") == "hell0 w0rld", "é-é".replaceAll("-", "+"))
console.log("я-я".replaceAll("я", "ab") == "ab-ab", "abc".replaceAll("b", "я"))
console.log("longer text with spaces".replaceAll(" ", ""))

// toUpperCase() and toLowerCase()
let all = ""
for (let c = 0; c < 256; c++) all += String.fromCharCode(c)
console.log(all.toUpperCase().split("").map((c) => c.charCodeAt(0)).join(","))
console.log(all.toLowerCase().split("").map((c) => c.charCodeAt(0)).join(","))
console.log("Hello".toUpperCase() == "HELLO", "HeLLo".toLowerCase() == "hello", "[@`{z]".toUpperCase())
console.log("Café".toUpperCase(), "ÇA".toLowerCase() == "ça", "Zoë".toUpperCase() == "ZOË")
console.log("MIXED Case Text That Is Long".toLowerCase(), "ÀÉÎõü long latin text".toUpperCase())
console.log("µÿß".toUpperCase())

// trim()
const spaced = ["", " ", "  a  ", "\t\n\v\f\r x \r\n", "\u00a0 nbsp \u00a0", "\ufeff bom \u2028", "no space"]
for (const s of [...spaced, "   a long string with spaces   "]) {
  console.log(`[${s.trim()}] [${s.trimStart()}] [${s.trimEnd()}]`, s.trim().length)
}
console.log(`  ${wide}  `.trim() == wide, (" x" + "y".repeat(20) + " ").trim().length)

// Characters one by one
const forms = ["inline", "a literal longer than eight", `${"rope part one, "}${"rope part two"}`, "两个字节", wide]
for (const s of [...forms, "café"]) {
  let codes = 0
  let spaces = 0
  let points = 0
  for (let i = 0; i < s.length; i++) {
    codes += s.charCodeAt(i)
    if (s[i] == " ") spaces++
  }
  for (const c of s) points += c.length
  console.log(codes, spaces, points, s.charAt(2), s.codePointAt(1), s.at(-1))
}

// Comparisons
const sorted = ["b", "a\u0000", "a", "", "ab", "abc", "abd", "ab\u0000", "zz", "é", "e", "я"]
sorted.push("apple pie and more", "apple pie", "Apple")
sorted.sort()
console.log(JSON.stringify(sorted))
console.log("a" < "a\u0000", "ab" < "abc", "abd" > "abc", "é" > "z", "я" > "é")
console.log("apple pie" < "apple pie and more", wide < `${wide}a`)
const nuls = "ab\u0000\u0000\u0000\u0000\u0000\u0000x"
console.log("ab" < nuls, nuls < "ab", "abcdefghi" < "abcdefghj", "abcdefgh" < "abcdefghi")
console.log("abcdefghi" < "abcdefgh", "b" > nuls)

// slice() and substring() around the lengths of the forms
const source = "0123456789abcdefghijklmnopqrstuvwxyz"
const ranges: [int, int][] = [[0, 0], [0, 1], [3, 11], [3, 12], [3, 15], [3, 16], [20, 36]]
ranges.push([30, 36], [0, 36], [-5, 36], [10, 2])
for (const [from, to] of ranges) {
  const a = source.slice(from, to)
  const b = source.substring(from, to)
  console.log(from, to, a, b, a.length, b.length, a == b)
}
const nine = "abcdefghi"
console.log(nine.slice(6), nine.slice(1, 9), nine.slice(-1), nine.substring(8, 3), nine.slice(0, 8) == "abcdefgh")
const short = "abcdefgh"
console.log(short.slice(1, 4), short.slice(-3), short.substring(5, 2), short.slice(3) == "defgh")
console.log(wide.slice(2, 5), wide.slice(1, 14).length)

// Numbers to strings and back
const doubles = [0, -0, 1, -1.5, 0.1 + 0.2, 1 / 3.0, 2 / 3.0, 5e-324, 1.7976931348623157e308, 1e21, 1e-7]
doubles.push(1.2e-7, 123456789012345680000, 9007199254740993, 18014398509481990, 0.000001, 1e300, 4.35)
for (const x of [...doubles, 0.07]) console.log(`${x}`, String(-x), x.toFixed(2))
const texts = ["12", "  42  ", "-0", "+7", ".5", "5.", "1e3", "1E-2", "0x1F", "1_000", "", " ", "12é", "١٢"]
texts.push("\u00a012\u2028", "123456789012345678901234567890", "1.7976931348623159e308", "2e-324")
for (const s of [...texts, "Infinity", "-Infinity", "0.1e1"]) {
  console.log(`[${s}]`, Number(s), parseInt(s), parseFloat(s), parseInt(s, 16))
}

// Long keys of a Map, from strings of every form with the same text
const counts = new Map<string, int>()
const keyText = "a key longer than eight"
const keys = [keyText, `${"a key "}${"longer than eight"}`, ("я" + keyText).slice(1)]
keys.push((keyText + "!").slice(0, keyText.length), "a key longer than eighty".slice(0, 23))
for (const k of keys) counts.set(k, (counts.get(k) ?? 0) + 1)
console.log(counts.size, counts.get(keyText), [...counts.keys()].join(";"))
