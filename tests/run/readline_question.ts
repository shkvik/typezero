// same-as-node
// question() of node:readline/promises writes the query to the output and
// gives the next line of stdin (readline_question.in).
import { createInterface } from "node:readline/promises"

const rl = createInterface({ input: process.stdin, output: process.stdout })
const name = await rl.question("Name? ")
console.log(`hello, ${name}!`)
rl.close()
