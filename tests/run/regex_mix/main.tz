// Regular expressions together with the rest of the language: regexes in
// fields and static fields of a class from another file, matches in generator
// methods and generic functions, matchAll as a lazy iterator (next(),
// helpers, a generic function over iterators), regexes and matches as
// generic values, in arrays and in Results, and Results made from matches.
import { Scanner, parseSetting, report } from "./patterns"

function firstMatch<T>(items: T[], re: RegExp, text: (x: T) => string): T | undefined {
  return items.find(x => re.test(text(x)))
}

function countAll<T>(it: Generator<T>) {
  return it.reduce((n, _) => n + 1, 0)
}

function hold<T>(value: T) {
  return { value }
}

const scanner = new Scanner("main")
console.log(scanner, Scanner.numbers)
console.log(scanner.wordsOf(report))
let total = decimal(0)
for (const n of scanner.numbersOf(report)) total += n
console.log(`total ${total}`, [...scanner.numbersOf("1 2.5")])

// A lazy iterator: kept in a variable, it holds the string and its own copy
// of the regex, and finds each match when it is asked for one.
const payers = report.matchAll(/(?<name>[A-Z]\w+) paid/g)
console.log(payers.next().value?.groups.name, payers)
console.log(countAll(payers), countAll(report.matchAll(/\d/g)))
const g = /a/g
const rest = "aaa".matchAll(g)
rest.next()
console.log(g.lastIndex, [...rest].length, g.lastIndex)
console.log(report.matchAll(/\d+\.\d+/g).map(m => m.index).toArray())
console.log(Array.from("a1b22".matchAll(/\d+/g), m => m[0].length))

// Results made from matches.
const settings = ["mode = fast", "level=3", "bad line"].map(parseSetting)
console.log(settings, Result.combine(settings.slice(0, 2)).map(all => all.length))
function upperValue(line: string) {
  return safeTry(function* () {
    const setting = yield* parseSetting(line)
    return ok(setting.value.toUpperCase())
  })
}
console.log(upperValue(" name = ann "), upperValue("?"))

// Regexes and matches as generic values, in arrays and in Results.
const date = /(?<y>\d{4})-(?<m>\d{2})/
const when = date.exec("due 2024-05")
console.log(hold(date), hold(when), [date, /x/g])
const year: Result<RegExpMatchArray, string> = when != undefined ? ok(when) : err("no date")
console.log(year.map(m => m.index))
console.log(firstMatch([new Scanner("a1"), new Scanner("b22")], /\d\d/, s => s.label))
console.log(firstMatch(["x", "y"], /\d/, s => s))
