// Json next to classes and regexes: what a Json holds is checked before a
// regex method takes it, and JSON.parse<T> builds no class, regex or match.
class User {
  constructor(public name: string) {}
}
interface Account {
  owner: User
}
interface Rule {
  pattern: RegExp
}
const j = JSON.parse('"abc"')
// expect-error: type 'Json' is not assignable to type 'string'; a Json may hold any JSON value: check what it holds first
console.log(/a/.test(j))
// expect-error: a Json has no method 'match' until a check shows what it holds
console.log(j.match(/a/))
// expect-error: 'split' takes a regex or a string, found 'Json'
console.log("abc".split(j))
// expect-error: JSON.parse<Account> cannot build a 'Account': 'User' is a class, and only its constructor makes one
const account = JSON.parse<Account>('{"owner": {"name": "x"}}')
// expect-error: JSON.parse<Rule> cannot build a 'Rule': a RegExp comes only from a literal or 'new RegExp'
const rule = JSON.parse<Rule>('{"pattern": {}}')
// expect-error: fromJson<RegExpMatchArray> cannot build a 'RegExpMatchArray': a match comes only from a regex
const match = fromJson<RegExpMatchArray>(j)
console.log(account, rule, match)
