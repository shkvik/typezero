// Dictionaries in classes up a chain and in interfaces: an inventory counts
// items in a Map, a shop adds prices in a Record, a ledger keeps entries by
// day and the days it was closed in a Set.
export interface Priced {
  prices: Record<string, decimal>
  price(name: string): decimal
}

export class Inventory {
  counts = new Map<string, int>()
  tags: Record<string, string[]> = {}
  add(name: string, n: int, ...tags: string[]) {
    this.counts.set(name, (this.counts.get(name) ?? 0) + n)
    for (const t of tags) (this.tags[t] ??= []).push(name)
  }
  take(name: string, n: int): int {
    const have = this.counts.get(name) ?? 0
    if (n > have) throw new RangeError(`only ${have} of '${name}'`)
    if (have == n) this.counts.delete(name)
    else this.counts.set(name, have - n)
    return have - n
  }
}

export class Shop extends Inventory implements Priced {
  prices: Record<string, decimal> = {}
  price(name: string): decimal {
    const p = this.prices[name]
    if (p == undefined) throw new TypeError(`no price for '${name}'`)
    return p
  }
  value(): decimal {
    let total: decimal = 0
    for (const [name, n] of this.counts) total += this.price(name) * n
    return total
  }
}

export class Ledger {
  byDay = new Map<Date, string[]>()
  closed = new Set<Date>()
  note(day: Date, text: string) {
    const list = this.byDay.get(day)
    if (list == undefined) this.byDay.set(day, [text])
    else list.push(text)
  }
}
