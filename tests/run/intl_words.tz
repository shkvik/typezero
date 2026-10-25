// same-as-node
// Words of languages (docs/intl.md), from the ICU of the system:
// Intl.RelativeTimeFormat, ListFormat and DisplayNames, and the languages
// themselves: getCanonicalLocales, supportedLocalesOf. Only what the ICU of
// Windows names as node does: it has no short and no dialect names of
// languages and regions ("US English").
console.log(new Intl.RelativeTimeFormat("ru").format(-3, "days"), new Intl.RelativeTimeFormat("ru", { style: "short" }).format(-3, "days"))
console.log(new Intl.RelativeTimeFormat("ru", { style: "narrow" }).format(-3, "day"), new Intl.RelativeTimeFormat("en").format(2, "hours"))
const auto = new Intl.RelativeTimeFormat("en", { numeric: "auto" })
console.log(auto.format(1, "day"), auto.format(-1, "day"), auto.format(0, "year"), auto.format(-2, "weeks"))
console.log(new Intl.RelativeTimeFormat("en").format(-1234.5, "seconds"), new Intl.RelativeTimeFormat("de").format(1, "quarter"))
console.log(new Intl.RelativeTimeFormat("en").formatToParts(-1234.5, "days"))
console.log(auto.formatToParts(-1, "day"), auto.formatToParts(3, "months"))
console.log(new Intl.RelativeTimeFormat("en").resolvedOptions())
console.log(new Intl.RelativeTimeFormat("en-u-nu-arab").resolvedOptions(), new Intl.RelativeTimeFormat("en-u-nu-arab").format(5, "minutes"))
console.log(new Intl.RelativeTimeFormat("ru"))

const list = ["a", "b", "c"]
console.log(new Intl.ListFormat("ru").format(list), new Intl.ListFormat("en").format(list))
console.log(new Intl.ListFormat("en", { type: "disjunction" }).format(list), new Intl.ListFormat("en", { type: "unit", style: "narrow" }).format(list))
console.log(new Intl.ListFormat("en").format([]), new Intl.ListFormat("en").format(["one"]), new Intl.ListFormat("en").format(["", "x"]))
console.log(new Intl.ListFormat("en").formatToParts(list))
console.log(new Intl.ListFormat("de", { style: "short" }).resolvedOptions(), new Intl.ListFormat("de"))

const regions = new Intl.DisplayNames("en", { type: "region" })
console.log(regions.of("US"), regions.of("419"), regions.of("XX"), new Intl.DisplayNames("en", { type: "region", fallback: "none" }).of("XX"))
console.log(new Intl.DisplayNames("ru", { type: "region" }).of("DE"), regions.of("us"))
console.log(new Intl.DisplayNames("en", { type: "currency" }).of("USD"), new Intl.DisplayNames("ru", { type: "currency" }).of("RUB"))
console.log(new Intl.DisplayNames("en", { type: "currency", fallback: "none" }).of("XYZ"), new Intl.DisplayNames("en", { type: "currency" }).of("xyz"))
console.log(new Intl.DisplayNames("en", { type: "language" }).of("de"), new Intl.DisplayNames("de", { type: "language" }).of("fr"))
console.log(new Intl.DisplayNames("en", { type: "language", languageDisplay: "standard" }).of("en-US"))
console.log(new Intl.DisplayNames("en", { type: "script" }).of("Latn"), new Intl.DisplayNames("en", { type: "script" }).of("latn"))
console.log(new Intl.DisplayNames("en", { type: "calendar" }).of("gregory"))
const fields = new Intl.DisplayNames("en", { type: "dateTimeField" })
console.log(fields.of("era"), fields.of("weekOfYear"), new Intl.DisplayNames("en", { type: "dateTimeField", style: "short" }).of("weekOfYear"))
console.log(new Intl.DisplayNames("ru", { type: "dateTimeField" }).of("month"))
console.log(regions.resolvedOptions(), new Intl.DisplayNames("en", { type: "language" }).resolvedOptions())
console.log(regions)

console.log(Intl.getCanonicalLocales(["EN-us", "ru", "en-US", "zh-hant-tw"]), Intl.getCanonicalLocales("DE"), Intl.getCanonicalLocales())
console.log(Intl.NumberFormat.supportedLocalesOf(["ru-RU-u-nu-arab", "xx", "EN-us", "de"]))
console.log(Intl.Collator.supportedLocalesOf("en-XX"), Intl.DateTimeFormat.supportedLocalesOf(["fr", "qq"], { localeMatcher: "lookup" }))
