// decimal(s) of a string that is not a number throws a SyntaxError, as
// BigInt("12abc") does (BigNumber.js throws too).
// expect-exit: 1
// expect-stderr: SyntaxError: Cannot convert 12abc to a decimal
const inputs: string[] = ["1.5", " -2e3 ", "0x1F", "12abc"]
for (const s of inputs) console.log(decimal(s))
