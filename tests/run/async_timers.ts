// same-as-node
// Timers, intervals and immediates: after each callback the nextTick
// callbacks and the microtasks run; in a timer, an immediate comes before a
// timer of 0 ms. The delays are far apart, so the order does not depend on
// how late the system wakes the program.
const log = (s: string) => console.log(s)
setTimeout(() => log("timeout 70"), 70)
setTimeout(() => log("timeout 10"), 10)
const cleared = setTimeout(() => log("never"), 20)
clearTimeout(cleared)
clearTimeout(undefined)
let n = 0
const interval = setInterval(() => {
  n++
  log(`interval ${n}`)
  if (n == 3) clearInterval(interval)
}, 120)
setTimeout(() => {
  log("timeout 40")
  setImmediate(() => log("immediate after timeout 40"))
  setTimeout(() => log("timeout 0 after timeout 40"), 0)
  process.nextTick(() => log("tick after timeout 40"))
  void Promise.resolve().then(() => log("promise after timeout 40"))
}, 40)
// Two timers of one duration run in the order they were set, each followed
// by its microtasks.
setTimeout(() => {
  log("first of two")
  void Promise.resolve().then(() => log("promise of the first"))
}, 500)
setTimeout(() => log("second of two"), 500)
// An immediate set by an immediate runs in the next turn of the loop.
setTimeout(() => {
  setImmediate(() => {
    log("immediate 1")
    setImmediate(() => log("immediate 3"))
  })
  const second = setImmediate(() => log("immediate 2"))
  log(`has ref: ${second.hasRef()}`)
  const quiet = setImmediate(() => log("immediate 4, unref'd, runs with the others"))
  quiet.unref()
  log(`has ref: ${quiet.hasRef()}`)
}, 560)
// An unref()'d timer does not keep the program alive.
setTimeout(() => log("never: unref'd"), 2000).unref()
const refreshed = setTimeout(() => log("refreshed timer"), 700)
setTimeout(() => {
  refreshed.refresh()
  log("refreshed")
}, 620)
