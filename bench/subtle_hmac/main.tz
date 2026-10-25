// 200 000 HMAC-SHA256 signatures of messages of 1 KiB with one key, as a server checks the
// requests it gets: 1000 at a time with Promise.all; the XOR of the first bytes of each.
const enc = new TextEncoder()
const key = await crypto.subtle.importKey("raw", enc.encode("a secret of the server"), { name: "HMAC", hash: "SHA-256" }, false, ["sign"])
const message = new Uint8Array(1024)
for (let i = 0; i < message.length; i++) message[i] = i & 0xff
let check = 0
for (let batch = 0; batch < 200; batch++) {
  const pending: Promise<ArrayBuffer>[] = []
  for (let i = 0; i < 1000; i++) {
    const m = message.slice()
    m.fill(i & 0xff, 0, 1)
    m.fill(batch & 0xff, 1, 2)
    pending.push(crypto.subtle.sign("HMAC", key, m))
  }
  for (const s of await Promise.all(pending)) check ^= new Uint8Array(s)[0] ?? 0
}
console.log(check)
