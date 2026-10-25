// What JavaScript's Intl rejects when the program runs, written in the code
// (docs/intl.md): a locale that is no language tag, a value that is not one
// of the option, a currency or a unit that is not one, missing options,
// digits out of range; and what tz does not have.

// expect-error: 'en_US' is not a language tag, and JavaScript would throw a RangeError
const a = new Intl.NumberFormat("en_US")
// expect-error: 'currncy' is not a value of the option 'style', and JavaScript would throw a RangeError; it is 'decimal', 'percent', 'currency' or 'unit'
const b = new Intl.NumberFormat("en", { style: "currncy" })
// expect-error: style 'currency' needs the option 'currency'
const c = new Intl.NumberFormat("en", { style: "currency" })
// expect-error: 'RUBL' is not a currency code
const d = new Intl.NumberFormat("en", { style: "currency", currency: "RUBL" })
// expect-error: 'parsec' is not a unit of Intl
const e = new Intl.NumberFormat("en", { style: "unit", unit: "parsec" })
// expect-error: maximumFractionDigits is from 0 to 100
const f = new Intl.NumberFormat("en", { maximumFractionDigits: 101 })
// expect-error: minimumFractionDigits is above maximumFractionDigits
const g = new Intl.NumberFormat("en", { minimumFractionDigits: 3, maximumFractionDigits: 1 })
// expect-error: roundingIncrement is one of 1, 2, 5, 10
const h = new Intl.NumberFormat("en", { roundingIncrement: 3 })
// expect-error: 'maxFractionDigits' is not an option of Intl.NumberFormat
const i = new Intl.NumberFormat("en", { maxFractionDigits: 2 })
// expect-error: 'sometimes' is not a value of the option 'useGrouping'
const j = new Intl.NumberFormat("en", { useGrouping: "sometimes" })
// expect-error: the option 'minimumIntegerDigits' is a number, found 'string'
const k = new Intl.NumberFormat("en", { minimumIntegerDigits: "2" })
// expect-error: 'x' is not a numberingSystem
const l = new Intl.NumberFormat("en", { numberingSystem: "x" })
// expect-error: Intl.NumberFormat takes the language as a string
const m = new Intl.NumberFormat(["en", "ru"])
// expect-error: Intl.NumberFormat takes its options as an object
const n = new Intl.NumberFormat("en", 5)
// expect-error: 'format' takes a number or a decimal, found 'string'; read the number first, e.g. 'decimal(s)'
const o = new Intl.NumberFormat("en").format("12")
// expect-error: 'formatRange' is not supported yet
const p = new Intl.NumberFormat("en").formatRange(1, 2)
// expect-error: style 'unit' needs the option 'unit'
const q = (5).toLocaleString("en", { style: "unit" })

// expect-error: 'Intl.PluralRules' is made with 'new'
const r = Intl.PluralRules("en")
// expect-error: 'selectRange' is not supported yet
const s = new Intl.PluralRules("en").selectRange(1, 2)
// expect-error: property 'select' of Intl.PluralRules can only be called
const t = new Intl.PluralRules("en").select

// expect-error: the option 'numeric' is a boolean, found 'string'
const u = new Intl.Collator("en", { numeric: "yes" })
// expect-error: 'x' is not a collation
const v = new Intl.Collator("en", { collation: "x" })
// expect-error: expected 2 arguments, but got 1
const w = new Intl.Collator("en").compare("a")
// expect-error: 'high' is not a value of the option 'sensitivity'
const x = "a".localeCompare("b", "en", { sensitivity: "high" })
// expect-error: 'NFX' is not a normalization form, and JavaScript would throw a RangeError
const y = "a".normalize("NFX")
// expect-error: 'line' is not a value of the option 'granularity'
const z = new Intl.Segmenter("en", { granularity: "line" })

// expect-error: 'decade' is not a unit of Intl.RelativeTimeFormat
const aa = new Intl.RelativeTimeFormat("en").format(1, "decade")
// expect-error: type 'int' is not assignable to type 'string'
const bb = new Intl.ListFormat("en").format([1, 2])
// expect-error: Intl.DisplayNames needs the option 'type'
const cc = new Intl.DisplayNames("en")
// expect-error: Intl.DisplayNames needs the option 'type'
const dd = new Intl.DisplayNames("en", { style: "long" })
// expect-error: 'en_GB' is not a language tag
const ee = Intl.getCanonicalLocales(["en", "en_GB"])
// expect-error: 'Intl.NumberFormat.foo' is not supported; of the classes of Intl, only 'supportedLocalesOf' is
const ff = Intl.NumberFormat.foo("en")
// expect-error: 'Intl.Locale' is not supported yet
const gg = new Intl.Locale("en")
// expect-error: cannot find type 'Intl.Bogus'
const hh: Intl.Bogus = 1
