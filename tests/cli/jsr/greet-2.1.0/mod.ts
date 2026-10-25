import { red } from "jsr:@std/fmt@^1.0.0/colors"
import { fmtVersion } from "jsr:@std/fmt@^1.0.0"

export function greet(name: string) { return red(`hi, ${name}`) + ` (fmt ${fmtVersion})` }
