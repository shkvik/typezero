let n = 5
// expect-error: a number cannot be a condition: 0 would count as false; compare it: 'n != 0'
if (n) console.log(n)
