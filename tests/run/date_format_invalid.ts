// same-as-node
// format() of an invalid date throws a RangeError, as in JavaScript, and so
// does a time zone ICU does not know; uncaught, the error ends the program.
// expect-exit: 1
// expect-stderr: RangeError: Invalid time zone specified: Mars/Base
const zones = ["Asia/Bishkek", "Mars/Base"]
for (const timeZone of zones)
  console.log(new Intl.DateTimeFormat("en-US", { timeZone, hour: "numeric" }).format(0))
