// same-as-node
// The types of Intl in annotations, as TypeScript names them: the classes,
// their options, what resolvedOptions() and formatToParts() give.
class Money {
  private fmt: Intl.NumberFormat
  constructor(currency: string) {
    this.fmt = new Intl.NumberFormat("en-US", { style: "currency", currency })
  }
  show(x: number): string { return this.fmt.format(x) }
}
console.log(new Money("EUR").show(3.5))
function grouping(g: string | boolean): string {
  const opts: Intl.NumberFormatOptions = { useGrouping: g, maximumFractionDigits: 1 }
  return new Intl.NumberFormat("en", opts).format(12345.67)
}
console.log(grouping(false), grouping("min2"), grouping(true))
const o: Intl.ResolvedCollatorOptions = new Intl.Collator("ru").resolvedOptions()
console.log(o.locale, o.numeric)
const parts: Intl.NumberFormatPart[] = new Intl.NumberFormat("en").formatToParts(1234)
console.log(parts.map(p => p.type).join(","))
const seg: Intl.Segmenter = new Intl.Segmenter("en", { granularity: "word" })
const data: Intl.SegmentData[] = [...seg.segment("go now")]
console.log(data.length)
const rule: Intl.LDMLPluralRule = new Intl.PluralRules("en").select(2)
console.log(rule)
function format(d: Date, options: Intl.DateTimeFormatOptions): string {
  return d.toLocaleDateString("en-US", options)
}
console.log(format(new Date(Date.UTC(2026, 9, 24)), { timeZone: "UTC", month: "long" }))
const names: Intl.DisplayNames = new Intl.DisplayNames("en", { type: "region" })
const named: string | undefined = names.of("FR")
console.log(named ?? "none")
function rank(words: string[], collator: Intl.Collator): string[] {
  return [...words].sort(collator.compare)
}
console.log(rank(["b", "a", "C"], new Intl.Collator("en")))
const listOptions: Intl.ListFormatOptions = { type: "disjunction" }
console.log(new Intl.ListFormat("en", listOptions).format(["tea", "coffee"]))
const resolved: Intl.ResolvedNumberFormatOptions = new Intl.NumberFormat("en", { notation: "compact" }).resolvedOptions()
console.log(resolved.notation, resolved.compactDisplay, resolved.minimumFractionDigits)
