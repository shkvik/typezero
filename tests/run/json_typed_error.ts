// JSON.parse<T> of a JSON that does not fit T throws a TypeError that names
// the path of the value; nobody catches it here, so the program ends with it.
// expect-exit: 1
// expect-stderr: TypeError: items[1].price: expected decimal, got string
type Order = { items: { price: decimal; count: int }[] }
const good = JSON.parse<Order>('{"items": [{"price": 1.5, "count": 2}]}')
console.log(good)
const bad = JSON.parse<Order>('{"items": [{"price": 1.5, "count": 2}, {"price": "2", "count": 1}]}')
console.log(bad)
