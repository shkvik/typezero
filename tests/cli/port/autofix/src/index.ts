// A library written for tsc and node; tz port makes a tz package of it.
import shout, { whisper } from "./voice"
import * as units from "./units.js"
import type { Options } from "./options"
import { type Level, levels } from "./options"
export { default as loud } from "./voice"
export { levels }

export default function greet(name: string, options?: Options): string {
  var level: Level = options === undefined ? "normal" : options.level
  if (level === "loud") return shout(name)
  if (level !== "normal") return whisper(name)
  return "hello, " + name + "!"
}

export function count(n: number, unit: string): string {
  return n + " " + units.plural(unit, n) + " (" + units.short(unit) + ")"
}

export function find(names: string[], name: string): string | null {
  const i = names.indexOf(name)
  return i === -1 ? null : names[i]
}
