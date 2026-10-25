// same-as-node
// readLine of tz:io reads stdin (io_readline.in) a line at a time, without
// its "\n" or "\r\n", and gives undefined at the end. Bytes that are not
// UTF-8 become U+FFFD; the last line may have no "\n".
import { readLine } from "tz:io"

const name = readLine("Имя? ")
console.log(`привет, ${name}!`)
let line: string | undefined
let count = 0
while ((line = readLine()) != undefined) {
  // expect-warning: 'count++' may overflow int: 'count' grows with every pass of the loop on line 11, which has no bound
  count++
  console.log(count, JSON.stringify(line), line.length)
}
console.log(readLine(), readLine("again? "))
