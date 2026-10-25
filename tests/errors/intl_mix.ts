// Two kinds of Intl objects in one union cannot be told apart when the
// program runs: the runtime makes them, with nothing a check could compare.
const nf = new Intl.NumberFormat("en")
// expect-error: 'Intl.NumberFormat' and 'Intl.Collator' cannot be told apart when the program runs: both are objects of Intl
const either: Intl.NumberFormat | Intl.Collator = process.argv.length > 9 ? new Intl.Collator("en") : nf
