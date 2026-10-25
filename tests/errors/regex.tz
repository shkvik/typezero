// The traps of JavaScript's regexes, and what is not known of a regex that
// is not a literal.
const s = "a-b-c"
const word = /a/g
// expect-error: 'test' of a regex with the 'g' flag starts where the previous match ended
word.test("a")
// expect-error: to replace all of them write 'replaceAll("-", ...)', for only the first one a regex: 'replace(/-/, ...)'
s.replace("-", "+")
const separator = "."
// expect-error: 'replace(new RegExp(RegExp.escape(separator)), ...)'
s.replace(separator, "+")
// expect-error: 'replaceAll' needs a regex with the g flag (JavaScript throws a TypeError without it), e.g. '/a/g'
"aaa".replaceAll(/a/, "b")
// expect-error: 'matchAll' needs a regex with the g flag
"aaa".matchAll(/a/)
// expect-error: to check for the text write 'includes(".")', for a regex '/\./'
"a1b".match(".")
// expect-error: to find the text write 'indexOf(".")'
"цена 1.5".search(".")
// expect-error: the regex has 3 groups, so there is no group 4 for '$4'
s.replace(/(a)-(b)-(c)/, "$4")
// expect-error: the regex has no group named 'x' for '$<x>'
s.replace(/(?<y>a)/, "$<x>")
// expect-error: the regex has no named groups, so JavaScript would insert '$<x>' as it is
s.replace(/(a)/, "$<x>")
// expect-error: without the u flag, a class takes a character written with two code units
const emoji = /[😀]/
// expect-error: invalid regular expression: unterminated group
const open = /(/
// expect-error: invalid regular expression: nothing to repeat; to match the character itself, escape it with '\'
const star = /a**/
// expect-error: invalid regular expression flags 'gg'
const flags = /a/gg
// expect-error: '\p' needs the u or v flag: without it, '\p{L}' is the text 'p{L}'
const letter = /\p{L}/
// expect-error: there is no such group to refer to (JavaScript would read an octal escape here)
const reference = /(a)\2/
// expect-error: invalid regular expression: invalid property name
const property = /\p{Foo}/u
// expect-error: invalid regular expression: unterminated character class
const dynamic = new RegExp("[a")
// expect-error: 'RegExp.$1' is a legacy property that holds what the last match of any regex found
const legacy = RegExp.$1
// expect-error: 'compile' is a legacy method that changes the regex in place
word.compile("b")

const date = /(?<year>\d{4})-(\d{2})(-\d{2})?/
const m = date.exec("2024-01")
if (m) {
  // expect-error: the regex has 3 groups (m[1] to m[3]; m[0] is the match), so there is no m[4]
  console.log(m[4])
  // expect-error: type 'string | undefined' is not assignable to type 'string'
  const day: string = m[3]
  // expect-error: property 'day' does not exist on type '{ year: string }'
  console.log(m.groups.day)
  // expect-error: a match has 'indices' only with the d flag
  console.log(m.indices)
}
const plain = /(a)/.exec("a")
// expect-error: the regex has no named groups; name one with '(?<name>...)'
console.log(plain?.groups)

// A regex from a parameter: its flags and groups are not known.
function find(re: RegExp, text: string) {
  // expect-error: what 'match' gives depends on the g flag
  const all = text.match(re)
  const one = re.exec(text)
  // expect-error: the named groups of a match are not supported yet when the regex is known only when the program runs
  console.log(one?.groups)
  // expect-error: the function gets only the match: the regex is known only when the program runs
  return text.replace(re, (all, group) => all)
}
// expect-error: 'replace' passes 5 arguments: the match, 2 groups, the offset and the string
s.replace(/(a)(b)/, (all, a, b, offset, text, extra) => all)
// expect-error: 'new RegExp' takes the pattern as a string
const copy = new RegExp(word)
// expect-error: a function as the replacement needs a regex: 'replaceAll(/-/g, ...)'
s.replaceAll("-", x => x)
// expect-error: 'split' takes a regex or a string, found 'int'
s.split(5)
// expect-error: an object cannot be converted to a string
const text = `${m}`
// expect-error: invalid regular expression: properties of strings are not supported yet
const emojiSet = /\p{RGI_Emoji}/v
