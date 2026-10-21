const words: string[] = []
for (let i = 0; i < 1000000; i++) words.push("w" + (i % 1000))
let n = 0
for (const w of words) if (w.endsWith("7")) n++
console.log(n)
