// Dates next to JSON and arrays: JSON has no dates, so JSON.parse<T> builds
// none (node gives the ISO string there); sort() without a comparison would
// compare the texts of toString().
type Saved = { title: string; at: Date }
// expect-error: JSON.parse<Saved> cannot build a 'Saved': JSON has no dates, and JSON.parse in JavaScript gives the string JSON.stringify wrote; read it as a 'string' and make the date with 'new Date(text)'
const saved = JSON.parse<Saved>('{"title": "x", "at": "2026-10-26T09:00:00.000Z"}')
const j = JSON.parse('["2026-10-26T09:00:00.000Z"]')
// expect-error: cannot build a 'Date[]': JSON has no dates
const dates = fromJson<Date[]>(j)
const list = [new Date(0), new Date(1000)]
// expect-error: sorting dates needs a comparison function: 'sort()' without one compares the texts of toString()
list.sort()
console.log(saved, dates, list.join())
