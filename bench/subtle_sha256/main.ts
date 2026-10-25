// 2 GiB through crypto.subtle.digest("SHA-256"): 128 pieces of 16 MiB, all at once with
// Promise.all, and the digest of their digests.
const piece = new Uint8Array(16 << 20)
for (let i = 0; i < piece.length; i++) piece[i] = i & 0xff
const pieces: Uint8Array[] = []
for (let i = 0; i < 128; i++) {
  const p = piece.slice()
  p.fill(i & 0xff, 0, 1)
  pieces.push(p)
}
const digests = await Promise.all(pieces.map(p => crypto.subtle.digest("SHA-256", p)))
const all = new Uint8Array(32 * digests.length)
digests.forEach((d, i) => all.set(new Uint8Array(d), 32 * i))
console.log(Buffer.from(await crypto.subtle.digest("SHA-256", all)).toString("hex"))
