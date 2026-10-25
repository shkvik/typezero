// same-as-node
// Tasks that push into one array keep node's order for the push, and only
// for it: what each computes before runs in parallel (docs/async.md,
// section 2). The array comes out in node's order on every run.
const results: string[] = []

async function convert(name: string) {
  await Promise.resolve()
  let text = ""
  for (let i = 0; i < 3; i++) text += name.toUpperCase()
  results.push(text)
}

await Promise.all(["a", "b", "c"].map((name) => convert(name)))
console.log(results)
