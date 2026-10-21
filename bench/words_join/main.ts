const words: string[] = []
for (let i = 0; i < 1000000; i++) words.push("w" + (i % 1000))
console.log(words.join(",").length)
