// Числовые типы и преобразования между ними.

// int — 32-битное целое. Литерал без подсказки тоже получает тип int.
let apples: int = 7
let boxes = 2
console.log("яблок на коробку:", apples / boxes) // деление целых: 3
console.log("остаток:", apples % boxes)

// long — 64-битное целое для больших чисел.
let population: long = 8100000000
console.log("население Земли:", population)

// double (он же number) — дробное число. int сам превращается в double.
let price: double = 12.5
let total: number = price * apples
console.log("итого:", total)

// Литерал берёт тип из контекста: здесь 7 / 2 делится как double.
let half: double = 7 / 2
console.log("7 / 2 в double:", half)

// Всё, что может потерять данные, пишется явно через `as`.
let whole = total as int
console.log("без дробной части:", whole)

let expensive: boolean = total > 50
console.log("дорого?", expensive)

// Переполнение int заворачивается, как в Go.
let max: int = 2147483647
console.log("max + 1 =", max + 1)
