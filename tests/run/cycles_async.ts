// same-as-node --experimental-transform-types
// Async code that holds objects in cycles: tasks that know their scheduler,
// which knows them; timers whose callbacks hold the objects; promises of
// objects in cycles.
class Scheduler {
  tasks: Task[] = []
  done: string[] = []
  add(name: string, delay: int) {
    const task = new Task(name, this)
    this.tasks.push(task)
    task.result = task.run(delay)
    return task
  }
}

class Task {
  result?: Promise<int>
  constructor(
    public name: string,
    public scheduler: Scheduler,
  ) {}
  async run(delay: int): Promise<int> {
    await new Promise<void>(resolve => setTimeout(resolve, delay))
    this.scheduler.done.push(this.name)
    return this.name.length
  }
}

class Peer {
  other?: Peer
  constructor(public name: string) {}
}

async function pair(name: string): Promise<Peer> {
  const a = new Peer(`${name}-a`)
  const b = new Peer(`${name}-b`)
  a.other = b
  b.other = a
  await Promise.resolve()
  return a
}

async function main() {
  const s = new Scheduler()
  s.add("slow", 20)
  s.add("quick", 5)
  s.add("middle", 10)
  let sum = 0
  for (const t of s.tasks) sum += (await t.result) ?? 0
  console.log(s.done.join(" "), sum)

  // A timer that holds a peer until it fires.
  const p = await pair("x")
  await new Promise<void>(resolve =>
    setTimeout(() => {
      console.log(p.other?.other?.name)
      resolve()
    }, 1),
  )
  const peers = await Promise.all([pair("y"), pair("z")])
  console.log(peers.map(q => q.other?.name).join(" "))
}

await main()
