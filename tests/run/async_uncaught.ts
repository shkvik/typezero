// same-as-node
// expect-exit: 1
// expect-stderr: RangeError: in a timer
// An error that a timer's callback throws ends the program at once: what
// its microtasks and the timers after it would print does not come.
setTimeout(() => {
  console.log("in the timer")
  void Promise.resolve().then(() => console.log("never: its microtask"))
  throw new RangeError("in a timer")
}, 1)
setTimeout(() => console.log("never: the next timer"), 5)
