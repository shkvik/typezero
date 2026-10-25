// same-as-node
// expect-stderr: to stderr
// writeFileSync and appendFileSync to the fds 1 and 2 write to stdout and
// stderr, in order with console.log.
import { writeFileSync, appendFileSync } from "node:fs"

for (let i = 0; i < 2; i++) {
  console.log("one", i)
  writeFileSync(1, "two\n")
  console.log("three")
  appendFileSync(1, "four, ünïcödé ✓\n")
}
writeFileSync(2, "to stderr\n")
