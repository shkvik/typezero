// same-as-node
// The u and v flags, properties, and case-insensitive matching by Unicode.
const text = "Ünïcödé 123 ٣٤٥ Ελληνικά русский 中文 😀👍🏽 ﬅﬆ"
console.log(text.match(/\p{L}+/gu), text.match(/\p{Nd}+/gu))
console.log(text.match(/\p{Script=Greek}+/u)?.[0], text.match(/\p{sc=Cyrl}+/u)?.[0], text.match(/\p{scx=Hani}+/u)?.[0])
console.log(text.match(/\p{Emoji_Presentation}/gu), text.match(/\P{ASCII}/gu)?.length, text.match(/[^\p{L}\s]+/gu))
console.log(/\p{Lu}/u.test("a"), /\p{Lu}/u.test("A"), /\p{General_Category=Uppercase_Letter}/u.test("Ω"))
console.log(/^\p{Any}$/u.test("😀"), /^\p{Assigned}$/u.test("͸"), /\p{White_Space}/u.test("　"))
console.log("x1 y22 z333".replace(/\p{Ll}(\p{N}+)/gu, "<$1>"), "ÄÖÜ".replace(/\p{Lu}/gu, c => c.toLowerCase()))

// /i by Unicode case folding with u and v; by toUpperCase without.
console.log(/σ/iu.test("Σ"), /σ/iu.test("ς"), /σ/i.test("ς"), /ς/i.test("Σ"))
console.log(/K/iu.test("k"), /K/i.test("k"), /k/iu.test("K"), /[a-z]/iu.test("K"))
console.log(/ſ/iu.test("S"), /ſ/i.test("s"), /ß/iu.test("ẞ"), /ß/i.test("ẞ"))
console.log(/\w/iu.test("ſ"), /\W/iu.test("ſ"), /\b/iu.test("K"), /[^\W]/iu.test("K"))
console.log(/ﬅ/iu.test("ﬆ"), /Ǆ/iu.test("ǆ"), /Ǆ/iu.test("ǅ"), /İ/iu.test("i"), /ı/i.test("I"))
console.log("ΣΑΣ σας".match(/σ+/giu), "ÄäÀà".match(/ä/gi), "STRASSE straße".match(/ss|ß/giu))
console.log(/(Ä)\1/iu.test("Ää"), /(σ)\1/iu.test("σς"), /(a)\1/i.test("aA"), /(K)\1/iu.test("Kk"))
console.log(/\P{Lu}/iu.test("A"), /\P{Lu}/iv.test("A"), /[^\P{Lu}]/iv.test("a"), /\p{Lu}/iv.test("a"))

// Surrogate pairs: one character with u, two code units without.
console.log("😀".length, /^.$/u.test("😀"), /^..$/.test("😀"), "a😀b".match(/./gu)?.length)
console.log(/\u{1F600}/u.test("😀"), /😀/u.test("😀"), /\uD83D/u.test("😀"), /\uD83D/.test("😀"))
console.log("😀😀".replace(/(?:)/gu, "|"), "😀😀".replace(/(?:)/g, "|").length, "😀a".split(/(?:)/u))
console.log(/[😀-😂]/u.test("😁"), /^[^x]$/u.test("😀"), /(?<=😀)b/u.test("😀b"), /\B/u.exec("S😀")?.index)
const surrogates = /\p{Cs}/u
console.log(surrogates.test("\uD800"), surrogates.test("😀"), "\uD800x".match(/\p{Cs}/gu)?.length)

// The v flag: set operations, strings in classes, and nested classes.
console.log("abcdef".match(/[\p{L}--[aeiou]]+/v)?.[0], "a1b2".match(/[\w--\d]/gv), "xyz".match(/[[a-z]&&[^y]]+/v)?.[0])
console.log("xabcy".match(/[\q{abc|ab|a}]/v)?.[0], "ΔΕδε".match(/[\q{δε}]/giv), /^[\q{}]$/v.test(""))
console.log(/[\p{ASCII}&&\p{L}]/v.test("é"), /[\p{L}&&\p{ASCII}]/v.test("e"), /[^[^x]]/v.test("x"))
console.log("a-b".match(/[\-]/v)?.[0], /[a\-z]/v.test("-"), /[\(\)]/v.test("("), /[\q{x\|y}]/v.test("x|y"))
console.log(/[\p{L}--\p{Lu}]/iv.test("A"), /[\p{L}--\p{Lu}]/v.source)
