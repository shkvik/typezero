// What tz port cannot change by itself: each gives a line of the list.
import { join } from "jsr:@std/path@^1.0.0/join"
import { parse } from "npm:yaml@2"
import { z } from "zod"
import "./setup.ts"
import * as helpers from "./helpers.ts"

export interface Point {
  x: number
  y: number
}

export class Box {
  #size = 0
  get size() { return this.#size }
  set size(v: number) { this.#size = v }
}

export function area(p: any): number {
  const point = p as Point
  return point.x * point.y
}

export const modes = ["a", "b"] as const
const config = { debug: true } satisfies Record<string, boolean>

export function pick(a: string): string
export function pick(a: number): number
export function pick(a: string | number): string | number {
  return a
}

export function legacy() {
  return arguments.length + helpers.count(Box.prototype)
}

export function load() {
  return require("fs")
}

export function where(dir: string) {
  return join(dir, "x") + parse("a: 1") + z + config.debug
}

export function ensure(p: unknown): asserts p is Point {}
