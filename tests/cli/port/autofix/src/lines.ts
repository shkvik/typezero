import { Readable } from "node:stream"

// The lines of a text as a stream of node, one chunk each.
export class Lines extends Readable {
  private left: string[]
  constructor(lines: string[]) {
    super()
    this.left = lines
  }
  _read() {
    const line = this.left.shift()
    this.push(line === undefined ? null : line + ".")
  }
}
