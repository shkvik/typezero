// same-as-node
// Tasks that touch only their own data run in parallel (docs/async.md,
// section 2). They wait on timers here, as requests would on the network; the
// parts and the page each makes are its own, which no other task sees.
const sleep = (ms: int) => new Promise<void>((resolve) => setTimeout(resolve, ms))

async function fetchPage(url: string) {
  await sleep(url.length)
  const parts = url.split("/")
  const page = { url, size: 0 }
  for (const part of parts) page.size += part.length
  return page.size
}

const urls = ["example.com/a", "example.com/b/c", "example.com"]
const sizes = await Promise.all(urls.map((url) => fetchPage(url)))
console.log(sizes)
