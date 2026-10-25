// same-as-node
// Errors and async: what an async function throws rejects its Promise, and
// `await` throws it again; catch(), then(f, g) and finally() as in Node;
// e.stack names the functions it came out of, "async" before those that
// awaited it.
function show(stack: string) {
  for (const line of stack.split("\n")) {
    const slash = Math.max(line.lastIndexOf("/"), line.lastIndexOf("\\"))
    const open = line.indexOf("(")
    const at = line.indexOf("at ")
    console.log(slash < 0 ? line
      : (open < 0 ? line.slice(0, at + 3) + (line.includes(" async ") ? "async " : "") : line.slice(0, open + 1)) +
        line.slice(slash + 1))
  }
}

async function fail(message: string): Promise<int> {
  await undefined
  throw new RangeError(message)
}

async function inner(n: int) {
  await undefined
  if (n > 0) throw new Error("deep")
  return n
}
async function middle(n: int) {
  const r = await inner(n)
  return r + 1
}

async function main() {
  try {
    await fail("first")
  } catch (e) {
    console.log("caught", e.name, e.message, e instanceof RangeError)
  } finally {
    console.log("finally")
  }
  console.log("recovered", await fail("second").catch(e => -1))
  console.log("then(f, g)", await fail("third").then(v => v * 2, e => e.message.length))
  console.log("then passes", await Promise.resolve(21).then(undefined, e => 0))
  try {
    await fail("through then").then(v => v + 1)
  } catch (e) {
    console.log("rejection passes through then:", e.message)
  }
  // An error in a callback rejects the Promise then() gives.
  try {
    await Promise.resolve(1).then(v => {
      if (v == 1) throw new TypeError("in callback")
      return v
    })
  } catch (e) {
    console.log("from callback:", e.name, e.message)
  }
  // finally() passes on what came, unless it throws itself.
  const kept = await Promise.resolve("kept").finally(() => console.log("finally callback"))
  console.log(kept)
  try {
    await Promise.resolve(1).finally(() => {
      throw new Error("finally threw")
    })
  } catch (e) {
    console.log(e.message)
  }
  // The executor of new Promise: what it throws rejects the Promise, but not
  // after it settled.
  try {
    await new Promise<int>(() => {
      throw new Error("executor threw")
    })
  } catch (e) {
    console.log(e.message)
  }
  console.log(await new Promise<int>(resolve => {
    resolve(7)
    throw new Error("ignored: settled already")
  }))
  console.log(await new Promise<int>((resolve, reject) => {
    reject(new Error("first wins"))
    resolve(8)
  }).catch(e => e.message.length))
}
await main()

// The stack of an error that comes through two awaits.
async function report() {
  try {
    await middle(1)
  } catch (e) {
    show(e.stack)
  }
}
void report()

// A rejected Promise that something handles later in the same turn is
// not reported.
const late = Promise.reject(new Error("handled later"))
process.nextTick(() => {
  late.catch(e => console.log("handled in nextTick:", e.message))
})
