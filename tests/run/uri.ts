// same-as-node
// encodeURIComponent, encodeURI, decodeURIComponent and decodeURI, with their
// URIErrors.

function attempt(what: string, f: () => string) {
  try {
    console.log(what, JSON.stringify(f()))
  } catch (e) {
    console.log(what, `${e}`, e instanceof URIError, e.name)
  }
}

// What each keeps: the unreserved characters, and encodeURI the reserved ones.
let ascii = ""
for (let i = 0; i < 128; i++) ascii += String.fromCharCode(i)
console.log(encodeURIComponent(ascii))
console.log(encodeURI(ascii))
const reserved = ";/?:@&=+$,#"
console.log(encodeURIComponent(reserved), encodeURI(reserved))
console.log(encodeURIComponent("-_.!~*'()"), encodeURI("-_.!~*'()"))
console.log(encodeURIComponent("a b&c=d/e?f#g"), encodeURI("http://x.org/a b?q=1&r=é#top"))

// UTF-8 of one, two, three and four bytes; Latin-1 strings and others.
console.log(encodeURIComponent("é ÿ \u0080  "), encodeURIComponent("привет"))
console.log(encodeURIComponent("भारत €"), encodeURIComponent("😀 𝄞"), encodeURI("😀"))
console.log(encodeURIComponent(""), encodeURIComponent("plain"), encodeURIComponent("%"))
console.log(encodeURIComponent(42), encodeURIComponent(1.5), encodeURIComponent(true))

// A lone surrogate has no UTF-8.
attempt("lone high", () => encodeURIComponent("a\uD800b"))
attempt("lone low", () => encodeURIComponent("\uDC00"))
attempt("high at the end", () => encodeURI("abc\uD83D"))
attempt("two highs", () => encodeURIComponent("\uD83D😀"))
attempt("reversed pair", () => encodeURIComponent("\uDE00\uD83D"))

// Decoding: mixed case hex, all lengths, and what decodeURI keeps.
console.log(decodeURIComponent("%c3%A9%C3%a9"), decodeURIComponent("%D0%BF%D1%80%D0%B8"))
console.log(decodeURIComponent("%E0%A4%AD%e2%82%ac"), decodeURIComponent("%F0%9F%98%80!"))
console.log(decodeURIComponent("%41%42c%20d"), decodeURIComponent("no escapes"),
  decodeURIComponent(""))
const escapedReserved = encodeURIComponent(reserved)
console.log(decodeURI(escapedReserved), decodeURIComponent(escapedReserved))
console.log(decodeURI("%2f%2F%3a%23%25%41%20%c3%a9"), decodeURI("a%2Bb+c"))
console.log(decodeURIComponent(encodeURIComponent(ascii)) == ascii)
console.log(decodeURI(encodeURI(ascii)) == ascii)
const mixed = "Ünïcödé 😀 текст & ;/?:@&=+$,# " + ascii
console.log(decodeURIComponent(encodeURIComponent(mixed)) == mixed)
console.log(decodeURI(encodeURI(mixed)) == mixed)

// Escapes that are not UTF-8, or not escapes at all.
const bad = ["%", "abc%", "%4", "%zz", "%G0", "%E0%A4%A", "%E0%A4", "%C3", "%C3%28", "%80",
  "%BF", "%C0%80", "%C1%BF", "%E0%80%80", "%ED%A0%80", "%ED%BF%BF", "%F0%80%80%80",
  "%F4%90%80%80", "%F5%80%80%80", "%F8%80%80%80%80", "%FF", "%E0%A4%AD%", "x%E2%82"]
for (const s of bad) {
  attempt(`component ${s}`, () => decodeURIComponent(s))
  attempt(`uri ${s}`, () => decodeURI(s))
}
attempt("reserved then bad", () => decodeURI("%2F%"))

// Long strings, ropes, and strings that share memory with others.
let long = ""
for (let i = 0; i < 300; i++) long += i % 7 == 0 ? "ü " : "a"
const encoded = encodeURIComponent(long)
console.log(encoded.length, encoded.slice(0, 40), decodeURIComponent(encoded) == long)
const wide = "€".repeat(100) + "x"
console.log(encodeURIComponent(wide).length, decodeURIComponent(encodeURIComponent(wide)) == wide)
const part = "prefix that is long enough: a%20b%20c".slice(28)
console.log(decodeURIComponent(part), encodeURIComponent(part))
console.log(["a b", "c/d", "é"].map(encodeURIComponent), ["a%20b"].map(decodeURIComponent))

// URIError is an Error like the others.
const made = new URIError("made")
console.log(made.name, made.message, made instanceof Error, `${made}`)
