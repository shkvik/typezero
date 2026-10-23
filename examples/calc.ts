// Калькулятор в обратной польской записи: "3 4 + 2 *" — это (3 + 4) * 2.
enum Op {
  Add,
  Sub,
  Mul,
  Div,
  Pow,
  Sqrt,
  Unknown,
}

function parseOp(token: string): Op {
  switch (token) {
    case "+":
      return Op.Add
    case "-":
      return Op.Sub
    case "*":
    case "×":
      return Op.Mul
    case "/":
      return Op.Div
    case "^":
      return Op.Pow
    case "sqrt":
      return Op.Sqrt
    default:
      return Op.Unknown
  }
}

// Значение выражения, или undefined с объяснением, если оно неверное.
function evaluate(expression: string): double | undefined {
  const stack: double[] = []
  for (const token of expression.split(" ")) {
    if (token == "") continue
    const value = Number(token)
    if (!isNaN(value)) {
      stack.push(value)
      continue
    }
    const op = parseOp(token)
    if (op == Op.Unknown) {
      console.log(`  непонятно, что такое "${token}"`)
      return undefined
    }
    const b = stack.pop()
    if (b == undefined) {
      console.log(`  для "${token}" не хватает чисел`)
      return undefined
    }
    if (op == Op.Sqrt) {
      stack.push(Math.sqrt(b))
      continue
    }
    const a = stack.pop()
    if (a == undefined) {
      console.log(`  для "${token}" не хватает чисел`)
      return undefined
    }
    switch (op) {
      case Op.Add:
        stack.push(a + b)
        break
      case Op.Sub:
        stack.push(a - b)
        break
      case Op.Mul:
        stack.push(a * b)
        break
      case Op.Div:
        stack.push(a / b)
        break
      default: // Op.Pow
        stack.push(a ** b)
    }
  }
  if (stack.length != 1) {
    console.log(`  в конце осталось чисел: ${stack.length}, а нужно одно`)
    return undefined
  }
  return stack[0]
}

const expressions = ["3 4 + 2 *", "2 10 ^", "2 sqrt", "1 3 /", "0.1 0.2 +", "5 0 /", "1 +", "2 3", "7 x"]
for (const expression of expressions) {
  const result = evaluate(expression)
  console.log(expression.padEnd(12), "=", result == undefined ? "ошибка" : String(result))
}
