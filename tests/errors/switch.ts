// switch, labels, break and continue.
const n: int = 1
switch (n) {
  // expect-error: a case of type 'string' cannot be compared with a 'switch' value of type 'int'
  case "1":
    break
}
switch (n) {
  case 1:
    let inFirst = 5
    break
  case 2:
    // expect-error: 'inFirst' is declared in an earlier case of the 'switch'; declare it before the 'switch'
    console.log(inFirst)
}
switch (n) {
  default:
    break
}
switch (n) {
  case 1:
    // expect-error: 'continue' can only be used inside a loop
    continue
}
for (;;) {
  // expect-error: cannot find label 'nowhere'
  break nowhere
}
block: {
  // expect-error: 'continue block' needs a loop, and 'block' does not label one
  continue block
}
// expect-error: duplicate label 'twice'
twice: twice: for (;;) {}
// expect-error: a declaration cannot have a label
named: let x = 1
