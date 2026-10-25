// same-as-node
// Dates in UTC: the time value and its fields, the setters, the ISO and UTC
// texts, Date.UTC, comparing and subtracting dates. A date is a reference: a
// setter changes it for everyone who holds it.
const d = new Date(1792692000000)
console.log(d)
console.log(d.getTime(), d.valueOf(), +d, Number(d))
console.log(d.toISOString(), d.toJSON(), d.toUTCString())
console.log(d.getUTCFullYear(), d.getUTCMonth(), d.getUTCDate(), d.getUTCDay())
console.log(d.getUTCHours(), d.getUTCMinutes(), d.getUTCSeconds(), d.getUTCMilliseconds())

// The setters change the date and give its new time.
const same = d
console.log(d.setUTCHours(25, 30), same)
console.log(d.setUTCDate(0), d)
console.log(d.setUTCMonth(13), d)
console.log(d.setUTCFullYear(2028, 1, 29), d)
console.log(d.setUTCMinutes(-1), d.setUTCSeconds(90, 500), d.setUTCMilliseconds(1234), d)
console.log(d.setUTCHours(1.9, 2.5), d.setUTCDate(31), d)
console.log(d.setTime(0), d, d.setTime(1.75), d.getTime())

// Date.UTC; new Date of a date makes a copy.
const t = Date.UTC(2026, 9, 23, 10, 30)
console.log(t, Date.UTC(2026), Date.UTC(2026, 0, 0), new Date(t))
const copy = new Date(d)
copy.setUTCFullYear(2000)
console.log(d, copy)
const months: int[] = [0, 11, 12, -1, 24]
for (const m of months) console.log(m, new Date(Date.UTC(2026, m, 15)))

// Comparing and subtracting dates.
const start = new Date(Date.UTC(2026, 0, 1))
const end = new Date(Date.UTC(2026, 11, 31, 23, 59, 59, 999))
console.log(end - start, (end - start) / 86400000, start < end, start >= end)
console.log(start == start, start == new Date(start), start.getTime() == new Date(start).getTime())
const dates = [end, start, new Date(Date.UTC(2026, 5, 15))]
dates.sort((a, b) => a - b)
console.log(dates)
console.log(dates.map(x => x.getUTCMonth()), dates.filter(x => x > start).length)

// Far years and the limits.
console.log(new Date(-62198755200000), new Date(253402300800000))
console.log(new Date(8.64e15), new Date(-8.64e15), new Date(8.64e15 + 1))
console.log(new Date(8.64e15).toUTCString(), new Date(-1e14).toUTCString())
console.log(new Date(-1).getUTCFullYear(), new Date(-1).getUTCMilliseconds())

// An invalid date: NaN for its numbers, as in JavaScript.
const bad = new Date(NaN)
console.log(bad, bad.getTime(), bad.getUTCDay(), bad.toUTCString(), [bad])
console.log(bad.setUTCHours(1), bad, bad.setUTCFullYear(2026), bad)

// Dates in objects, classes, arrays and closures.
interface Event {
  name: string
  at: Date
}
class Meeting {
  title: string
  at: Date
  constructor(title: string, at: Date) {
    this.title = title
    this.at = at
  }
  postpone(days: int) {
    this.at.setUTCDate(this.at.getUTCDate() + days)
  }
}
const events: Event[] = [
  { name: "start", at: start },
  { name: "end", at: end },
]
const meeting = new Meeting("plan", new Date(Date.UTC(2026, 9, 23, 9)))
meeting.postpone(10)
console.log(events, meeting, meeting.at.getUTCDate())
const later = (days: double) => new Date(start.getTime() + days * 86400000)
console.log(later(31), [later(1), later(365)])
console.log({ nested: { deeper: { deepest: { at: start } } } })
