// A package from a directory: uuid, adapted for tz (tests/packages/uuid), with what is
// deterministic: v3 and v5 of fixed names, v1, v4, v6 and v7 of fixed options (time, random
// bytes), parse and stringify, validate and version, the conversions and the errors.
import {
  v1, v3, v4, v5, v6, v7, NIL, MAX, DNS, URL, parse, stringify, validate, version, v1ToV6, v6ToV1,
} from "uuid"

const random = new Uint8Array([
  0x10, 0x91, 0x56, 0xbe, 0xc4, 0xfb, 0xc1, 0xea, 0x71, 0xb4, 0xef, 0xe1, 0x67, 0x1c, 0x58, 0x36,
])
console.log(NIL, MAX, DNS, URL)

// Names in namespaces: the same every time, as RFC 9562 has them.
console.log(v3("hello.example.com", DNS), v3("http://example.com/hello", URL))
console.log(v5("hello.example.com", DNS), v5("http://example.com/hello", URL))
console.log(v5("hello", "1b671a64-40d5-491e-99b0-da01ff1f3341"), v3("", DNS), v5("Привет", URL))
const buf = new Uint8Array(20)
console.log(v5(new Uint8Array([1, 2, 3]), DNS, buf, 2), version(stringify(buf, 2)))

// Time and randomness given: v1, v4, v6, v7 are fixed.
const time = { msecs: 1321651533573, nsecs: 5432, clockseq: 0x385c, node: new Uint8Array([0x61, 0xcd, 0x3c, 0xbb, 0x32, 0x10]) }
console.log(v1(time), v1({ msecs: 0, random }), v1({ random, msecs: 1700000000000, nsecs: 9999 }))
console.log(v4({ random: random.slice() }), v4({ rng: () => random.slice() }))
console.log(v6(time), v6({ msecs: 1700000000000, random }))
console.log(v7({ msecs: 1700000000000, seq: 0x12345678, random }), v7({ msecs: 0, random }), v7({ msecs: 0xffffffffffff, seq: 0xffffffff, random }))
const out = new Uint8Array(16)
console.log(v7({ msecs: 1, random }, out), v4({ random: random.slice() }, out))

// parse and stringify, validate and version.
const id = "6ec0bd7f-11c0-43da-975e-2a8ad9ebae0b"
const bytes = parse(id)
console.log(bytes, stringify(bytes) == id, validate(id), validate("6ec0bd7f-11c0-43da-975e-2a8ad9ebae0"), validate(NIL), validate(MAX))
console.log(validate("6EC0BD7F-11C0-43DA-975E-2A8AD9EBAE0B"), validate("6ec0bd7f-11c0-03da-975e-2a8ad9ebae0b"), validate(42))
for (const u of [id, v1(time), v3("x", DNS), v5("x", URL), v6(time), v7({ msecs: 5, random })]) console.log(version(u))

// v1 and v6 into each other.
const one = v1(time)
const six = v1ToV6(one)
console.log(six, v6ToV1(six), v6ToV1(six) == one)
console.log(v1ToV6(parse(one)), v6ToV1(parse(v6(time))))

// The defaults: the time of now and random bytes.
const fresh = [v1(), v4(), v6(), v7()]
for (const u of fresh) console.log(validate(u), version(u), stringify(parse(u)) == u)
const a = v7(), b = v7()
console.log(a < b, v1() != v1())

// Errors, as uuid throws them.
function attempt(run: () => void) {
  try {
    run()
  } catch (e) {
    console.log(e.name, e.message)
  }
}
attempt(() => { parse("not a uuid") })
attempt(() => { stringify(new Uint8Array(16).fill(0xff)) })
attempt(() => { version("x") })
attempt(() => { v4({ random: new Uint8Array(8) }) })
attempt(() => { v4({}, new Uint8Array(8), 0) })
attempt(() => { v5("x", DNS, new Uint8Array(16), 4) })
attempt(() => { v3("x", new Uint8Array(4)) })
