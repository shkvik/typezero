// same-as-node
// env: TZ=UTC
// What dates throw when the program runs, caught: toISOString(), format()
// and formatToParts() of an invalid date (toJSON gives null), and a time
// zone, a value of an option, a calendar, fractionalSecondDigits or a
// language that only the running program shows to be wrong.
function attempt(what: string, f: () => string) {
  try {
    console.log(what, f())
  } catch (e) {
    console.log(what, e instanceof RangeError, e instanceof TypeError, `${e}`)
  }
}
const texts = ["2026-10-23T10:00Z", "not a date"]
for (const text of texts) attempt(`iso ${text}`, () => new Date(text).toISOString())
for (const text of texts) attempt(`json ${text}`, () => JSON.stringify({ at: new Date(text) }))
const format = new Intl.DateTimeFormat("en-US", { timeZone: "UTC", hour: "numeric" })
for (const text of texts) attempt(`format ${text}`, () => format.format(new Date(text)))
for (const text of texts)
  attempt(`parts ${text}`, () => format.formatToParts(new Date(text)).map((p) => p.value).join(""))
const zones = ["Asia/Bishkek", "Mars/Base"]
for (const timeZone of zones)
  attempt(`zone ${timeZone}`, () =>
    new Intl.DateTimeFormat("en-US", { timeZone, hour: "numeric" }).format(0),
  )
for (const timeZone of zones)
  attempt(`locale zone ${timeZone}`, () => new Date(0).toLocaleString("en-US", { timeZone }))
const months = ["long", "huge"]
for (const month of months)
  attempt(`month ${month}`, () =>
    new Date(0).toLocaleDateString("en-US", { month, timeZone: "UTC" }),
  )
const calendars = ["gregory", "not a calendar!"]
for (const calendar of calendars)
  attempt(`calendar ${calendar}`, () =>
    new Intl.DateTimeFormat("en-US", { calendar, timeZone: "UTC" }).format(0),
  )
const digits: int[] = [2, 7]
for (const fractionalSecondDigits of digits)
  attempt(`digits ${fractionalSecondDigits}`, () =>
    new Date(1234).toLocaleTimeString("en-US", { fractionalSecondDigits, timeZone: "UTC" }),
  )
const tags = ["de-DE", "en_US"]
for (const tag of tags)
  attempt(`tag ${tag}`, () => new Date(0).toLocaleDateString(tag, { timeZone: "UTC" }))
console.log(new Date(NaN).toLocaleString(), String(new Date(NaN)), JSON.stringify([new Date(NaN)]))

// An argument that throws: the date, the formatter and the language already
// evaluated are released on the way out.
function hours(n: int): int {
  if (n > 2) throw new RangeError(`too many: ${n}`)
  return n
}
function zone(n: int): string {
  if (n > 2) throw new TypeError(`no zone ${n}`)
  return "UTC"
}
const start = () => new Date(Date.UTC(2026, 0, 1))
const utc = new Intl.DateTimeFormat("en-US", { timeZone: "UTC" })
for (const n of [1, 3]) {
  attempt("set", () => `${start().setUTCHours(hours(n))}`)
  attempt("locale", () => start().toLocaleString(`en-${n > 0 ? "US" : "GB"}`, { timeZone: zone(n) }))
  attempt("new", () => new Intl.DateTimeFormat("en-US", { timeZone: zone(n) }).format(start()))
  attempt("format", () => utc.format(new Date(hours(n))))
}
