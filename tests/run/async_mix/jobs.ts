// The tasks main.ts runs: async methods up a class chain and through an
// interface, a generic cache of promises in a Map, values that may be
// promises, and ResultAsync over node:fs.
import { readFileSync, writeFileSync } from "node:fs"
import { join } from "node:path"
import { ResultAsync } from "neverthrow"

export function later<T>(value: T, ms: number): Promise<T> {
  return new Promise(resolve => setTimeout(() => resolve(value), ms))
}

export interface Source {
  read(key: string): Promise<string>
}

export class Job {
  runs = 0
  last: Promise<string> | undefined = undefined
  constructor(public name: string) {}
  async step(input: string): Promise<string> {
    return input
  }
  async run(input: string) {
    this.runs++
    const out = await this.step(input)
    return `${this.name}: ${out}`
  }
  start(input: string): Promise<string> {
    this.last = this.run(input)
    return this.last
  }
}

export class Upper extends Job {
  override async step(input: string) {
    await later(0, 2)
    return input.toUpperCase()
  }
}

export class Twice extends Upper {
  override async step(input: string) {
    const once = await super.step(input)
    return `${once} ${once}`
  }
}

// Files in a directory, read by sync calls inside async code.
export class Folder implements Source {
  constructor(public dir: string) {}
  async read(key: string) {
    await undefined
    return readFileSync(join(this.dir, key), "utf8")
  }
  write(key: string, text: string) {
    writeFileSync(join(this.dir, key), text)
  }
}

export class Memory implements Source {
  data = new Map<string, string>()
  read(key: string): Promise<string> {
    const found = this.data.get(key)
    return found == undefined ? Promise.reject(new Error(`no ${key}`)) : later(found, 1)
  }
}

// One promise for each key, shared by whoever asks again.
export class Cache {
  pending = new Map<string, Promise<int>>()
  loads = 0
  constructor(public load: (key: string) => Promise<int>) {}
  get(key: string): Promise<int> {
    const known = this.pending.get(key)
    if (known != undefined)
      return known
    this.loads++
    const p = this.load(key)
    this.pending.set(key, p)
    return p
  }
}

// The same for any value: a function that keeps its promises in a Map.
export function memo<T>(load: (key: string) => Promise<T>): (key: string) => Promise<T> {
  const pending = new Map<string, Promise<T>>()
  return key => {
    let p = pending.get(key)
    if (p == undefined) {
      p = load(key)
      pending.set(key, p)
    }
    return p
  }
}

// What a cached value or a fresh promise may be.
export function quick(n: int): int | Promise<int> {
  return n % 2 == 0 ? n * 10 : later(n * 100, 1)
}

export function readText(source: Source, key: string): ResultAsync<string, string> {
  return ResultAsync.fromPromise(source.read(key), e =>
    e instanceof SystemError ? `${e.code} ${e.syscall}` : e.message)
}
