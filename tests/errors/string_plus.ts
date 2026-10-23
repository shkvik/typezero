// A number or a boolean joined with a string by `+` is an error: JavaScript
// turns it into text, and `count + input` with input from argv joins where
// adding was meant. Text is built with templates; the hint gives the template.
const total = 42
const input = "8"
const flag = true
let s = "a"
// expect-error: operator '+' cannot be applied to types 'string' and 'int'
// expect-error: use a template: '`Итого: ${total}`'
console.log("Итого: " + total)
// expect-error: to add, convert the string: 'total + Number(input)'; to join, use a template: '`${total}${input}`'
console.log(total + input)
// expect-error: to add, convert the string: 'Number(input) + total'
console.log(input + total)
// expect-error: types 'string' and 'boolean' (JavaScript would join them as text); use a template: '`ok: ${flag}`'
console.log("ok: " + flag)
// expect-error: convert it with 'String(total)'
console.log("" + total)

// A chain is reported once, with all its parts in the template; the numbers
// before the first string add up, as in JavaScript.
// expect-error: use a template: '`x = ${total}, y = ${total * 2}!`'
console.log("x = " + total + ", y = " + total * 2 + "!")
// expect-error: use a template: '`${total + 1} items`'
console.log(total + 1 + " items")
// expect-error: use a template: '`(${s}) ${total}`'
console.log("(" + s + ") " + (total))
// Quotes stay as they are; a backquote and `${` are escaped in a template.
// expect-error: use a template: '`it\'s ${total}`'
console.log('it\'s ' + total)
// expect-error: use a template: '`cost \${x}, \`q\`: ${total}`'
console.log("cost ${x}, `q`: " + total)

// `+=` too.
// expect-error: operator '+=' cannot be applied to types 'string' and 'int'
// expect-error: write 's += String(5)'
s += 5
// expect-error: write 's += String(total + 1)'
s += (total + 1)
// expect-error: write 's += String(flag)'
s += flag

// A value that may be undefined, but is a number when it is there.
function show(n?: int) {
  // expect-error: types 'string' and 'int | undefined'
  return "n = " + n
}
console.log(show(1), `${total}`, "a" + String(total), s)
