// Reading the input: regexes made when the program runs, decimal(s) and
// JSON.parse<T>, whose errors become errors of the program with a cause.
import { AppError, ParseError } from "./errors.ts"

export type Entry = { name: string; amount: decimal; tags: string[] }

export function lineRegex(separator: string): RegExp {
  return new RegExp(`^(\\w+)${separator}\\s*(\\S+)(.*)$`)
}

export function tagsOf(text: string, flags: string): string[] {
  const tags: string[] = []
  for (const m of text.matchAll(new RegExp("#(\\w+)", flags))) tags.push(m[1] ?? "")
  return tags
}

export function parseLine(line: string, n: int, separator: string, flags: string): Entry {
  const m = lineRegex(separator).exec(line)
  if (m == undefined) throw new ParseError(n, `cannot read '${line}'`)
  let amount: decimal = 0
  try {
    amount = decimal(m[2] ?? "")
  } catch (e) {
    throw new ParseError(n, `bad amount '${m[2]}'`, e)
  }
  return { name: m[1] ?? "", amount, tags: tagsOf(m[3] ?? "", flags) }
}

export type Config = { limit: decimal; rules: string[]; owner?: { name: string } }

export function loadConfig(text: string): Config {
  try {
    return JSON.parse<Config>(text)
  } catch (e) {
    throw new AppError("bad config", 500, { cause: e })
  }
}

// Reading a Json by hand: a key of null throws a TypeError.
export function ownerName(text: string): string {
  const j = JSON.parse(text)
  const name = j.owner.name
  return typeof name == "string" ? name : "?"
}
