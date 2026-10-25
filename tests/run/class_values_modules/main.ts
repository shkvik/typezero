// Mixins and a decorator that replaces a class, across files: the class a
// copy of a mixin makes belongs to the file that has the mixin, and the
// name a file exports means the class the decorator gave.
import { CountedNote, Note, show } from "./model"
import { created } from "./mixin"

const a = new Note("a")
const b = new CountedNote("b")
console.log(a, b, show(a), show(b), created, b.serial)
const kinds: Constructor<Note>[] = [Note, CountedNote]
console.log(kinds.map(k => new k("k").tag), created)
