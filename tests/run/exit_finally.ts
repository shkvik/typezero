// same-as-node
// expect-exit: 3
// process.exit() ends the program at once: a finally around it does not
// run, as in Node (docs/io.md).
try {
  console.log("exiting")
  process.exit(3)
} finally {
  console.log("not printed")
}
