// Чек из магазина: разбор текста, числа из строк и форматирование.
type Money = double

const input = `
  Молоко 3,2%;  2; 89.90
  Хлеб бородинский; 1; 54
  Сыр;0.35;899
  Яблоки голден ; 1.2 ; 129.5
  Кофе; две пачки; 650
`

// Без второго аргумента скидки нет.
function withDiscount(sum: Money, percent: double = 0): Money {
  return sum * (1 - percent / 100)
}

function money(sum: Money): string {
  return sum.toFixed(2).replace(/\./, ",") + " ₽"
}

const names: string[] = []
const sums: Money[] = []
for (const raw of input.split("\n")) {
  const line = raw.trim()
  if (line == "") continue
  const parts = line.split(";")
  if (parts.length != 3) {
    console.log("не понял строку:", line)
    continue
  }
  // После проверки длины parts[0], parts[1] и parts[2] — просто строки.
  const amount = Number(parts[1].trim())
  const price = Number(parts[2].trim())
  if (isNaN(amount) || isNaN(price)) {
    console.log(`пропускаю "${parts[0].trim()}": количество "${parts[1].trim()}" — не число`)
    continue
  }
  names.push(parts[0].trim())
  sums.push(amount * price)
}

const width = 40
const line = "".padEnd(width, "─")
console.log(line)
console.log("чек".toUpperCase().padStart(width / 2 + 1))
console.log(line)
let total: Money = 0
for (let i = 0; i < names.length; i++) {
  const name = names[i]
  const sum = sums[i] ?? 0
  total += sum
  console.log(name.padEnd(width - 14, ".") + money(sum).padStart(14, "."))
}
console.log(line)
console.log("Итого:".padEnd(width - 14) + money(total).padStart(14))
const discounted = withDiscount(total, 5)
console.log("Со скидкой 5%:".padEnd(width - 14) + money(discounted).padStart(14))
console.log(`Округлим до рубля: ${Math.round(discounted)} ₽`)
