// The other file of regex_mix: regexes in a class (in a field and in a
// static field), a generator method over matches, a function that returns
// Results, and a text.
export class Scanner {
  static numbers = /-?\d+(?:\.\d+)?/g
  words = /\p{L}+/gu
  constructor(public label: string) {}

  wordsOf(text: string) {
    return [...text.matchAll(this.words)].map(m => m[0])
  }

  // The numbers of the text as exact decimals, one at a time.
  *numbersOf(text: string) {
    for (const m of text.matchAll(Scanner.numbers)) yield decimal(m[0])
  }
}

export function parseSetting(line: string): Result<{ key: string; value: string }, string> {
  const m = /^\s*(?<key>[a-z_]+)\s*=\s*(?<value>.*?)\s*$/.exec(line)
  if (m == undefined) return err(`not a setting: '${line}'`)
  return ok({ key: m.groups.key, value: m.groups.value })
}

export const report = "Ann paid 19.99 and 0.01; Bob paid -5 and 2.5"
