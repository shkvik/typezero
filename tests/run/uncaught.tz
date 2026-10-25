// same-as-node
// expect-exit: 1
// expect-stderr: TypeError: no config
// An error nobody catches: what is printed so far stays, the error and its
// stack go to stderr, and the program ends with 1, as in Node.
function load(): string {
  throw new TypeError("no config")
}
console.log("start")
console.log(load())
console.log("not reached")
