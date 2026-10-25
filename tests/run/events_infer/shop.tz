import { EventEmitter } from "node:events"

// A class that extends EventEmitter has the events its emits give it.
export class Shop extends EventEmitter {
  sell(what: string, count: int) {
    this.emit("sold", what, count)
  }
}

export function report(shop: Shop) {
  shop.emit("sold", "pear", 1)
}
