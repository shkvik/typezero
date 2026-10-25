// Decimals in the formats of languages (docs/intl.md, docs/decimal.md): ICU
// formats a decimal from its text, so every digit stays, as node formats a
// string of digits (a number of JavaScript would lose them).
const price = decimal("12345678901234567890.125")
console.log(price.toLocaleString("en-US"))
console.log(price.toLocaleString("ru-RU", { style: "currency", currency: "RUB" }))
console.log(decimal("0.1").toLocaleString("de", { minimumFractionDigits: 3 }))
console.log(decimal("-2.5").toLocaleString("en", { maximumFractionDigits: 0 }))
console.log(decimal("2.5").toLocaleString("en", { maximumFractionDigits: 0, roundingMode: "halfEven" }))
const precise = new Intl.NumberFormat("en", { maximumFractionDigits: 30 })
console.log(precise.format(decimal("1.000000000000000000000000000001")))
console.log(precise.formatToParts(decimal("-1234.5")))
console.log([decimal("1.5"), decimal("2.25")].map(new Intl.NumberFormat("fr").format))
console.log(new Intl.PluralRules("en").select(decimal("1")), new Intl.PluralRules("ru").select(decimal("2.5")))
const total = decimal("0.1") + decimal("0.2")
console.log(total.toLocaleString("en", { style: "percent" }), new Intl.NumberFormat("en", { notation: "compact" }).format(decimal("98765432109876543210")))
