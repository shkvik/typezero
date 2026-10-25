// same-as-node
// toISOString() of an invalid date throws a RangeError, as in JavaScript;
// uncaught, it ends the program.
// expect-exit: 1
// expect-stderr: RangeError: Invalid time value
const texts = ["2026-10-23T10:00Z", "not a date"]
for (const text of texts) console.log(new Date(text).toISOString())
