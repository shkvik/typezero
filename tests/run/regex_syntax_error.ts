// same-as-node
// new RegExp(s) of a pattern that is not a regex throws a SyntaxError, with
// the message of node; uncaught, it ends the program.
// expect-exit: 1
// expect-stderr: SyntaxError: Invalid regular expression: /a(b/i: Unterminated group
const patterns: string[] = ["a+", "[0-9]{2}", "a(b"]
for (const p of patterns) console.log(new RegExp(p, "i").test("A12"))
