export type Level = "quiet" | "normal" | "loud"

export interface Options {
  level: Level
}

export const levels: Level[] = ["quiet", "normal", "loud"]

// What a config file holds.
export type Raw = Json | null
export const nothing: Json = null
