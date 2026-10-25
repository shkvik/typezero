// same-as-node
// console.log of Json values, as node shows what JSON.parse gives.
const deep = JSON.parse(
  '{"a":{"b":{"c":{"d":1}},"list":[[[[1]]]]},"n":[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,100,1000]}')
console.log(deep)
console.log(JSON.parse('["apple","banana","cherry","date","elderberry","fig","grape","honeydew"]'))
const parts: string[] = []
for (let i = 0; i < 120; i++) parts.push(String(i))
console.log(JSON.parse(`[${parts.join(",")}]`))
console.log(JSON.parse(`{"long text":"${"x".repeat(100)}","k":1}`))
console.log(JSON.parse("[]"), JSON.parse("{}"), JSON.parse('""'), JSON.parse('"text"'), JSON.parse("null"),
  JSON.parse("true"), JSON.parse("-0"))
console.log([JSON.parse('"in an array"')], { j: JSON.parse('"in an object"') })
console.log(JSON.parse("[1.5, -2, 3e-7, 1e21, 123456789012345678901234567890]"))
console.log(JSON.parse('{"a":[true,false,null],"b":"it\'s \\"quoted\\"\\n","c d":{}}'))
console.log(JSON.parse('{"__proto__":1,"2":"two","x-y":3,"_ok":4,"$no":5}'))

// An object that holds itself; the cycle is broken again for the leak check.
const self = JSON.parse('{"name":"loop"}')
self.me = self
console.log(self)
self.me = undefined
const list = JSON.parse("[1]")
list[1] = list
console.log(list)
if (Array.isArray(list)) list.pop()
