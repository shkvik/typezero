// 200 thousand timers of 1 to 10 ms, of which every fourth is cleared at
// once: the lists of timers by duration, and the loop that waits for them.
let fired = 0
let cleared = 0
const timers: NodeJS.Timeout[] = []
for (let i = 0; i < 200000; i++)
  timers.push(setTimeout(() => {
    fired++
  }, 1 + (i % 10)))
for (let i = 0; i < timers.length; i += 4) {
  clearTimeout(timers[i])
  cleared++
}
await new Promise<void>(resolve => setTimeout(resolve, 11))
console.log(fired + cleared)
