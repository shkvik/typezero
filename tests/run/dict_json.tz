// JSON.parse<Record<string, T>> and fromJson: the keys in the order of the
// text; and JSON.stringify of dictionaries.
type Prices = Record<string, decimal>
const prices = JSON.parse<Prices>('{"tea": 2.50, "cake": 4, "jam": 0.1}')
console.log(prices, prices.tea, Object.keys(prices))
interface Item { name: string; tags: Record<string, boolean> }
const item = JSON.parse<Item>('{"name": "box", "tags": {"red": true, "big": false}}')
console.log(item, item.tags.red, item.tags["small"])
const nested = JSON.parse<Record<string, Record<string, int>>>('{"a": {"x": 1}, "b": {}}')
console.log(nested)
const optional = JSON.parse<Record<string, int | undefined>>('{"a": 1, "b": null}')
console.log(optional, "b" in optional)
try {
  JSON.parse<Record<string, int>>('{"a": 1, "b": "two"}')
} catch (e) {
  console.log(e.message)
}
try {
  JSON.parse<Record<string, int>>('[1, 2]')
} catch (e) {
  console.log(e.message)
}
const j = JSON.parse('{"x": [1, 2], "y": [3]}')
const lists = fromJson<Record<string, int[]>>(j)
console.log(lists, lists.x?.length)
console.log(JSON.stringify(prices), JSON.stringify(new Map([["a", 1]])), JSON.stringify(new Set([1])))
const withUndefined: Record<string, int | undefined> = { a: 1, b: undefined }
console.log(JSON.stringify(withUndefined), withUndefined)
console.log(JSON.stringify({ m: new Map<string, int>(), r: { k: [1] } }))
