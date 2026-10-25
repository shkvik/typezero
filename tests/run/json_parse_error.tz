// same-as-node
// JSON.parse of a text that is not JSON throws the SyntaxError node throws.
// expect-exit: 1
// expect-stderr: SyntaxError: Expected ',' or '}' after property value in JSON at position 13 (line 3 column 3)
console.log(JSON.parse('{"ok": [1, 2]}'))
const text = '{\n  "a": 1\n  "b": 2\n}'
console.log(JSON.parse(text))
