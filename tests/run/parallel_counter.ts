// same-as-node
// Closures that change a variable they capture share it: the pieces that
// change it keep node's order with each other (docs/async.md, section 2).
async function countAll(items: string[]) {
  let done = 0
  let total = 0
  await Promise.all(items.map(async (item) => {
    await Promise.resolve()
    const size = item.length * 2
    done++
    total += size
  }))
  return `${done} items, ${total} bytes`
}
console.log(await countAll(["ab", "cde", "f"]))

// Code that awaits each call itself runs one call after another: nothing
// runs at the same time as its pieces.
let ticks = 0
async function ticker() {
  for (let i = 0; i < 3; i++) {
    await Promise.resolve()
    ticks++
  }
}
await ticker()
await ticker()
console.log(ticks)
