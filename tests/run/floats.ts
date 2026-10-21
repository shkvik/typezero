// Numbers print like JavaScript's Number#toString.
console.log(0.1 + 0.2)
let third: double = 1 / 3
console.log(third)
console.log(100.0, 1e21, 1e-7, 123e-20)
console.log(0.000001, 1.2345678901234568e20)
console.log(-0.0, 5e-324, 1.7976931348623157e308)

let zero: double = 0
console.log(zero / zero, 1 / zero, -1 / zero)
console.log(2.5 % 1, -7.5 % 2)

// `as` truncates toward zero and saturates instead of overflowing.
let f: double = 1.9
console.log(f as int, -f as int)
console.log((zero / zero) as int, 1e300 as int, -1e300 as long)
let big: long = 9007199254740993
console.log(big as double)
