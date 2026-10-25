// Dates in classes, interfaces and errors: an event has the moment it starts,
// a meeting extends it, a deadline is due at a date.
export interface Dated {
  at: Date
}

export class Event implements Dated {
  constructor(public title: string, public at: Date) {}
  describe(): string {
    return `${this.title} at ${this.at.toISOString()}`
  }
  // The same event a number of days later: a new date, the old one stays.
  later(days: int): Event {
    return new Event(this.title, new Date(this.at.getTime() + days * 86400000))
  }
}

export class Meeting extends Event {
  constructor(title: string, at: Date, public minutes: int) {
    super(title, at)
  }
  describe(): string {
    return `${super.describe()} for ${this.minutes} min, until ${this.ends().toISOString()}`
  }
  ends(): Date {
    return new Date(this.at.getTime() + this.minutes * 60000)
  }
  later(days: int): Event {
    return new Meeting(this.title, new Date(this.at.getTime() + days * 86400000), this.minutes)
  }
}

export class LateError extends RangeError {
  constructor(public due: Date, public now: Date) {
    super(`late by ${(now.getTime() - due.getTime()) / 3600000} h`)
  }
}

export function checkDue(due: Date, now: Date): Date {
  if (now > due) throw new LateError(due, now)
  return due
}

// The moment a text names, or an error that says which text.
export function parseWhen(text: string): Date {
  const d = new Date(text)
  if (Number.isNaN(d.getTime())) throw new TypeError(`not a date: '${text}'`)
  return d
}
