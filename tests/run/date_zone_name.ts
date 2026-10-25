// same-as-node
// env: TZ=Etc/GMT-6
// The texts of local time end with the name of the zone, which for a zone
// without names is its offset in every language. (Etc/GMT-6 is UTC+6.)
const d = new Date(2026, 9, 23)
console.log(d.toString())
console.log(d.toTimeString())
console.log(d.toDateString(), d.getTimezoneOffset(), d)
console.log(`${new Date(Date.UTC(1969, 6, 20, 20, 17))}`)
console.log(new Intl.DateTimeFormat().resolvedOptions().timeZone)
