import { Entity, Log } from "./log.ts"

@Entity("orders")
export class Order {
  constructor(public total: double) {}

  @Log
  withTax(rate: double) {
    return this.total * (1 + rate)
  }
}
