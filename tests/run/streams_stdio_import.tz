// same-as-node
// stdin and stdout of node:process, by the names it gives them: the same
// streams as process.stdin and process.stdout.
import { stdin, stdout as out } from "node:process"
import { Readable } from "node:stream"

const chunks: string[] = []
stdin.setEncoding("utf8")
stdin.on("data", (s) => chunks.push(s))
stdin.on("end", () => {
  const text = chunks.join("")
  console.log("lines", text.split("\n").length - 1, stdin == process.stdin)
  Readable.from(["piped ", "to ", "stdout\n"]).pipe(out)
})
