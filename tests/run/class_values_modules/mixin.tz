export let created = 0
export function Counted<B extends Constructor>(Base: B) {
  return class extends Base {
    serial = ++created
  }
}
export function WithTag<T extends Constructor>(cls: T) {
  return class extends cls {
    tag = "tagged"
  }
}
