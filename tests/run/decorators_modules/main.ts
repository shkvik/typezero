// Decorators and metadata across files: a decorator of one file on a class
// of another, read by a function of the first.
import { tableOf } from "./log.ts"
import { Order } from "./model.ts"

const order = new Order(100)
console.log(order.withTax(0.25), tableOf(Order), tableOf(order))
