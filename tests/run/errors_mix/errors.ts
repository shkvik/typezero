// The errors of the program: classes that extend Error and its kinds, with
// fields of their own and a cause, and rules whose overriding methods throw.
export class AppError extends Error {
  constructor(message: string, public code: int, options?: { cause?: Error }) {
    super(message, options)
    this.name = "AppError"
  }
  describe(): string {
    return `${this.name} ${this.code}: ${this.message}`
  }
}

export class ParseError extends AppError {
  constructor(public line: int, message: string, cause?: Error) {
    super(`line ${line}: ${message}`, 400, { cause })
    this.name = "ParseError"
  }
  describe(): string {
    return `${super.describe()} (line ${this.line})`
  }
}

// A kind of its own of RangeError: its name stays "RangeError".
export class LimitError extends RangeError {
  constructor(public limit: decimal) {
    super(`over the limit of ${limit}`)
  }
}

export abstract class Rule {
  abstract check(value: decimal): decimal
  label(): string {
    return "rule"
  }
}

export class Positive extends Rule {
  check(value: decimal): decimal {
    if (value < 0) throw new AppError(`negative amount ${value}`, 422)
    return value
  }
  label(): string {
    return "positive"
  }
}

export class AtMost extends Rule {
  constructor(public max: decimal) {
    super()
  }
  check(value: decimal): decimal {
    if (value > this.max) throw new LimitError(this.max)
    return value
  }
}

// Through an interface: a regex made from its fields when the method runs.
export interface Validator {
  validate(text: string): string
}

export class Pattern implements Validator {
  constructor(public source: string, public flags: string) {}
  validate(text: string): string {
    const re = new RegExp(this.source, this.flags)
    if (!re.test(text)) throw new AppError(`'${text}' does not match`, 422)
    return text
  }
}
