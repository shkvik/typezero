import { Counted, WithTag } from "./mixin"
@WithTag
export class Note {
  constructor(public text: string) {}
}
export const CountedNote = Counted(Note)
export function show(n: Note) {
  return `${n.text} [${n.tag}]`
}
