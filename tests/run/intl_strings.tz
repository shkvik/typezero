// same-as-node
// Strings by the rules of languages (docs/intl.md), from the ICU of the
// system: localeCompare and Intl.Collator, normalize, Intl.Segmenter.
console.log(["ёж", "яблоко", "арбуз"].sort((a, b) => a.localeCompare(b, "ru")))
console.log("é".normalize("NFD").length)

// Sorting: compare as a value is bound to its collator.
const names = ["Émile", "zoe", "Zoë", "adam", "Ådne", "Øyvind"]
console.log([...names].sort(new Intl.Collator("en").compare))
console.log([...names].sort(new Intl.Collator("sv").compare))
console.log(["b", "a", "B", "A", "ä", "10", "9"].sort(new Intl.Collator("en", { numeric: true, caseFirst: "upper" }).compare))
console.log(["b", "a", "B", "A"].sort(new Intl.Collator("en", { caseFirst: "lower" }).compare))
const base = new Intl.Collator("en", { sensitivity: "base" }).compare
console.log(base("a", "A"), base("a", "á"), base("a", "b"), base("b", "a"))
const accent = new Intl.Collator("en", { sensitivity: "accent" })
console.log(accent.compare("a", "A"), accent.compare("a", "á"))
const kase = new Intl.Collator("en", { sensitivity: "case" })
console.log(kase.compare("a", "A"), kase.compare("a", "á"))
console.log(new Intl.Collator("en", { ignorePunctuation: true }).compare("a-b", "ab"))
console.log(["a", "b", "A"].map(x => "a".localeCompare(x, "en", { sensitivity: "base" })))
console.log("a".localeCompare("b", "en"), "b".localeCompare("a", "en"), "a".localeCompare("a", "en"))
console.log(["file10", "file2", "file1"].sort((a, b) => a.localeCompare(b, "en", { numeric: true })))
console.log(["Ä", "Ae", "Af"].sort(new Intl.Collator("de-u-co-phonebk").compare), ["Ä", "Ae", "Af"].sort(new Intl.Collator("de").compare))

// What a collator resolved to.
console.log(new Intl.Collator("en").resolvedOptions())
console.log(new Intl.Collator("th").resolvedOptions().ignorePunctuation)
console.log(new Intl.Collator("de-u-co-phonebk-kn").resolvedOptions())
console.log(new Intl.Collator("de-u-co-phonebk", { collation: "eor" }).resolvedOptions().locale)
console.log(new Intl.Collator("de-u-co-phonebk", { usage: "search" }).resolvedOptions())
console.log(new Intl.Collator("en-u-kf-upper", { caseFirst: "lower" }).resolvedOptions())
console.log(new Intl.Collator("en-u-kn", { numeric: false }).resolvedOptions().locale)
console.log(new Intl.Collator("en-u-kn-bogus").resolvedOptions().locale, new Intl.Collator("en-u-co-standard").resolvedOptions().locale)
console.log(new Intl.Collator("en", { collation: "phonebk" }).resolvedOptions().collation)
console.log(new Intl.Collator("ru"), Intl.Collator("en").compare("a", "b"))

// normalize: the four forms; text that needs nothing stays the same.
for (const form of ["NFC", "NFD", "NFKC", "NFKD"]) {
  const f = form == "NFC" ? "NFC" : form == "NFD" ? "NFD" : form == "NFKC" ? "NFKC" : "NFKD"
  console.log(form, "ﬁ é Å ①".normalize(f).length, "é".normalize(f).length, "abc".normalize(f))
}
console.log("é".normalize() == "é".normalize("NFC"), "é" == "é".normalize())
console.log("Å".normalize("NFD").split("").map(c => c.charCodeAt(0)))

// Segmenter: letters as people see them, words and sentences.
console.log([...new Intl.Segmenter("en").segment("👨‍👩‍👧é")].map(s => s.segment))
const text = "🇷🇺🇺🇸 👍🏽!"
console.log([...new Intl.Segmenter("en").segment(text)].length, text.length)
console.log([...new Intl.Segmenter("en", { granularity: "word" }).segment("Hi, you")])
console.log([...new Intl.Segmenter("en", { granularity: "sentence" }).segment("Hi. You! Ok?")])
const words = new Intl.Segmenter("ru", { granularity: "word" })
for (const w of words.segment("Привет, мир! 42 кота"))
  if (w.isWordLike) console.log(w.segment, w.index)
let count = 0
for (const s of words.segment("one two three four")) {
  if (!s.isWordLike) continue
  count++
  if (s.segment == "three") break
}
console.log(count, Array.from(words.segment("x y"), s => s.segment.toUpperCase()))
const japanese = new Intl.Segmenter("ja", { granularity: "word" }).segment("今日は良い天気です")
console.log([...japanese].map(s => s.segment).join("|"))
console.log(japanese.containing(3))
const abc = new Intl.Segmenter("en").segment("abc")
console.log(abc.containing(-1), abc.containing(3), abc.containing(1.7), abc.containing())
console.log(new Intl.Segmenter("en"), abc, new Intl.Segmenter("en-u-nu-arab").resolvedOptions())
console.log(new Intl.Segmenter("fr", { granularity: "word" }).resolvedOptions())
