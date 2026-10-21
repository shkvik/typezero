// Строки работают как в JavaScript: длина и индексы считаются в единицах UTF-16.
const text = "Съешь же ещё этих мягких французских булок, да выпей чаю"
const vowels = "аеёиоуыэюяАЕЁИОУЫЭЮЯ"

let count = 0
for (let i = 0; i < text.length; i++) {
  if (vowels.includes(text[i])) count++
}
console.log(`гласных: ${count} из ${text.length} символов`)

// += в цикле не копирует строку целиком на каждом шаге.
let reversed = ""
for (let i = text.length - 1; i >= 0; i--) reversed += text[i]
console.log(reversed)

let table = ""
for (let n = 1; n <= 5; n++) table += `${n} × ${n} = ${n * n}\n`
console.log(table.slice(0, -1))

const word = "булок"
const at = text.indexOf(word)
console.log(`"${word}" начинается с позиции ${at}:`, text.slice(at, at + word.length + 1))
console.log(text.startsWith("Съешь"), text.endsWith("чаю"), "ab".repeat(3))
