// What main.ts serves with: a class of handlers that a decorator registers
// by path, found and called through reflection; responses signed with
// HMAC; and bodies that an async generator streams.
import { createHmac, timingSafeEqual } from "node:crypto"

export const SECRET = "shared secret"
export const routes = new Map<string, string>()

// `@Route("/path")`: the method answers that path.
export function Route(path: string) {
  return (_cls: object, name: string) => {
    routes.set(path, name)
  }
}

export function sign(body: string): string {
  return createHmac("sha256", SECRET).update(body).digest("hex")
}

export function verified(body: string, signature: string): boolean {
  const expected = Buffer.from(sign(body), "hex")
  const given = Buffer.from(signature, "hex")
  return expected.length == given.length && timingSafeEqual(expected, given)
}

// Chunks of text, one at a time, as an async generator gives them.
export async function* counting(upTo: int) {
  for (let i = 1; i <= upTo; i++) {
    await undefined
    yield new TextEncoder().encode(`${i};`)
  }
}

export class Handlers {
  served = 0

  @Route("/hello")
  hello(req: Request): Response {
    this.served++
    const name = new URL(req.url).searchParams.get("name") ?? "world"
    const body = `hello ${name}`
    return new Response(body, { headers: { "x-signature": sign(body) } })
  }

  @Route("/count")
  count(_req: Request): Response {
    this.served++
    return new Response(new ReadableStream({
      start: async c => {
        for await (const chunk of counting(5)) c.enqueue(chunk)
        c.close()
      },
    }))
  }

  // The head at once, and a body that comes too late.
  @Route("/slow")
  slow(_req: Request): Response {
    this.served++
    return new Response(new ReadableStream({
      start: async c => {
        c.enqueue(new TextEncoder().encode("first;"))
        await new Promise<void>(resolve => setTimeout(resolve, 2000))
        c.enqueue(new TextEncoder().encode("too late"))
        c.close()
      },
    }))
  }
}
