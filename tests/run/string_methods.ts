// same-as-node
// More methods of strings.
const csv = "red,green,,blue"
console.log(csv.split(","), csv.split(",", 2), csv.split(""), "".split(","), "".split(""))
console.log("a--b--c".split("--"), csv.split(), "abc".split("x"), "x,y".split(",", 0))
const long = "one sentence, then another sentence, then a third sentence"
const pieces = long.split(", ")
console.log(pieces.length, pieces[2] ?? "")

console.log(`[${"  padded \t\n".trim()}]`, `[${"  start".trimStart()}]`, `[${"end  ".trimEnd()}]`)
console.log(`[${" ﻿wide spaces　 ".trim()}]`, `[${"   ".trim()}]`)

console.log("7".padStart(3, "0"), "7".padEnd(3, "-"), "abc".padStart(8, "12"), "abc".padEnd(2), "x".padStart(4))
console.log("long text".padStart(20, "."), "toolong".padEnd(3, "!"), "abc".padStart(6, ""))

console.log("Hello, World".toUpperCase(), "Hello, World".toLowerCase(), "ÀÉÎõü".toUpperCase(), "ÀÉÎõü".toLowerCase())
console.log("straße".toUpperCase(), "ǅ".toLowerCase(), "ǅ".toUpperCase(), "ﬁ".toUpperCase(), "İ".toLowerCase().length)
console.log("ΣΊΣΥΦΟΣ".toLowerCase(), "Σ".toLowerCase(), "ΑΣ ΣΑ".toLowerCase(), "привет мир".toUpperCase())
console.log("ÿ".toUpperCase(), "µ".toUpperCase(), "𐐨".toUpperCase(), "already lower".toLowerCase())

console.log("a.b.c".replace(".", "/"), "a.b.c".replaceAll(".", "/"), "abc".replace("x", "y"))
console.log("abc".replace("", "-"), "abc".replaceAll("", "-"), "aaa".replaceAll("aa", "b"))
console.log("price: 5".replace("5", "$$5"), "cat".replace("a", "[$&]"), "abc".replace("b", "($`|$')"), "x".replace("x", "$1$"))

console.log("hello".charAt(1), `[${"hello".charAt(9)}]`, "hello".charAt(), "😀x".codePointAt(0), "😀x".codePointAt(1), "😀x".codePointAt(2))
const text = "a😀b"
for (let i = 0; i < text.length; i++) console.log(i, text.codePointAt(i))
console.log("abc".codePointAt(5), "hello".lastIndexOf("l"), "hello".lastIndexOf("l", 2), "hello".lastIndexOf("x"), "hello".lastIndexOf(""))
console.log("abcabc".lastIndexOf("abc", 2), "abc".lastIndexOf("abc", -5), "aaa".lastIndexOf("a", 10))

console.log("con".concat("cat"), "a".concat("b", "c", "d"), "x".concat())
console.log(String.fromCharCode(72, 105, 33), String.fromCharCode(0x10041), String.fromCharCode(), String.fromCharCode(1.9 * 50))
console.log(String.fromCodePoint(0x1f600, 65), String.fromCodePoint(), String.fromCodePoint(0x410).length)
