// same-as-node --experimental-transform-types
// Observers: a subject holds its listeners, functions whose environments
// hold the subject back, and objects that hold a subject and are listening
// to it with their own methods.
type Listener = (value: int) => void

class Subject {
  listeners: Listener[] = []
  value = 0
  constructor(public name: string) {}
  subscribe(listener: Listener) {
    this.listeners.push(listener)
    return () => {
      const at = this.listeners.indexOf(listener)
      if (at >= 0) this.listeners.splice(at, 1)
    }
  }
  set(value: int) {
    this.value = value
    for (const l of this.listeners) l(value)
  }
}

class Display {
  shown: string[] = []
  constructor(public subject: Subject) {
    subject.subscribe(v => this.show(v))
  }
  show(v: int) {
    this.shown.push(`${this.subject.name}=${v}`)
  }
}

class Logger {
  lines = 0
  log(v: int) {
    this.lines += v
  }
}

let seen = 0
for (let round = 0; round < 100; round++) {
  const s = new Subject(`s${round}`)
  // A closure that holds the subject it listens to.
  s.subscribe(v => {
    if (s.value != v) console.log("wrong")
    seen++
  })
  const display = new Display(s)
  const logger = new Logger()
  // An object that a listener calls.
  const stop = s.subscribe(v => logger.log(v))
  s.set(1)
  s.set(2)
  stop()
  s.set(3)
  if (round == 99) console.log(display.shown.join(" "), logger.lines, s.listeners.length)
}
console.log(seen)

// A subject that outlives one of its displays.
const kept = new Subject("kept")
function attach() {
  const d = new Display(kept)
  kept.set(5)
  return d.shown.length
}
console.log(attach(), kept.listeners.length)
kept.set(6)
console.log(kept.value)
