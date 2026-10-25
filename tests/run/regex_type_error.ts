// same-as-node
// matchAll and replaceAll with a regex without g, known only when the program
// runs, throw the TypeError of node.
// expect-exit: 1
// expect-stderr: TypeError: String.prototype.matchAll called with a non-global RegExp argument
function count(re: RegExp, text: string): int {
  return [...text.matchAll(re)].length
}
console.log(count(/a/g, "banana"))
// The iterator of matchAll checks the flag when it is made, before any match
// is asked for, as in JavaScript.
function matchesOf(re: RegExp, text: string) {
  return text.matchAll(re)
}
const matches = matchesOf(new RegExp("a"), "banana")
console.log("not reached", matches)
