# A module with a declaration file next to it (docs/modules.md, section 7):
# box.d.tz holds what box.tz exports, with comments and examples, and tz
# checks that the two agree. A file that agrees builds; one that does not is
# an error at each place where they part.
include(${SOURCE}/../cli.cmake)

write(box.tz [[
export type Pair = {
  a: int;
  b?: string;
};

export function make(a: int): Pair {
  const pair = {
    a: a,
  };

  return pair;
}

class Secret {
  public readonly n: int;

  public constructor(n: int) {
    this.n = n;
  }
}

export class Box {
  public readonly value: int;
  private hidden: int;

  public constructor(value: int) {
    this.value = value;
    this.hidden = 0;
  }

  public twice(): int {
    return this.value * 2;
  }

  public static of(value: int): Box {
    const box = new Box(value);

    return box;
  }

  private secret(): int {
    return this.hidden;
  }
}

export class Shown {
  public constructor(secret: Secret) {}

  public ok(): boolean {
    return true;
  }
}
]])
write(box.d.tz [[
/** A pair of a number and, maybe, a text. */
export type Pair = {
  a: int;
  b?: string;
};

/**
 * Makes a pair.
 * @example
 * const p = make(1);
 */
export declare function make(a: int): Pair;

/** A box with a number. */
export declare class Box {
  /** What is in the box. */
  public readonly value: int;

  public constructor(value: int);

  /** Twice the value. */
  public twice(): int;

  public static of(value: int): Box;
}

/** Its constructor takes a class that programs cannot name, so it is left out. */
export declare class Shown {
  public ok(): boolean;
}
]])
write(main.tz [[
import { Box, make } from "./box"
console.log(Box.of(2).twice(), make(3).a)
]])
tz(run main.tz STDOUT "4 3\n")

write(box.d.tz [[
export type Pair = {
  a: int;
  b: string;
};

export declare function make(a: double): Pair;

export declare class Box {
  public value: int;
  readonly twice(): int;
  public static of(value: int): string;
  public secret(): int;
  public missing(): void;
}

export declare function ghost(): void;
]])
tz(run main.tz EXIT 1 STDERR
  "box.d.tz:1:13: error: field 'b' of 'Pair' is 'string' here, and 'string | undefined' in 'box.tz'"
  "box.d.tz:6:30: error: the type of parameter 'a' of 'make' is 'double' here, and 'int' in 'box.tz'"
  "box.d.tz:16:25: error: 'ghost' is declared here, but 'box.tz' does not export it"
  "box.d.tz:9:10: error: 'value' of 'Box' is readonly in 'box.tz'"
  "box.d.tz:10:12: error: write the access of 'twice' of 'Box': public, protected or private"
  "box.d.tz:11:17: error: what 'of' of 'Box' gives is 'string' here, and 'Box' in 'box.tz'"
  "box.d.tz:12:10: error: 'secret' of 'Box' is private in 'box.tz': a declaration file declares only what programs can use"
  "box.d.tz:13:10: error: 'Box' has no member 'missing' in 'box.tz'"
  "box.tz:26:10: error: 'constructor' of 'Box' is a member that programs can use, but 'box.d.tz' does not declare it"
  "box.tz:46:14: error: 'Shown' is exported, but 'box.d.tz' does not declare it: a declaration file declares all that its module exports")

write(box.d.tz [[
export type Pair = { a: int; b?: string };
const x = 1;
export declare function make(a: int): Pair {
  return { a };
}
]])
tz(run main.tz EXIT 1 STDERR
  "box.d.tz:2:1: error: a declaration file holds only what its module exports"
  "box.d.tz:3:25: error: a function of a declaration file has no body: its code is in the module")
