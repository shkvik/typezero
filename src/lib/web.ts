// The events of the web platform that node has as globals: EventTarget,
// Event, CustomEvent, and AbortController and AbortSignal on top of them,
// DOMException; ported from lib/internal/event_target.js and
// lib/internal/abort_controller.js of Node 24 (docs/events.md, section 4).
// A file that names one of them gets it, as if it imported it (see the
// loader, src/driver/loader.cpp).
//
// tz has no getters, so what node computes in a getter is a field here that
// the code keeps up to date: `currentTarget` and `eventPhase` while an event
// is dispatched, `returnValue` and `cancelBubble`. The fields node does not
// print (its getters print nothing) are marked `@__tz_hidden`; `__tz_*` are
// intrinsics of the compiler, which only its built-in modules can use.
//
// An AbortSignal is also what the runtime cancels its waits with (the
// timers of node:timers/promises, node:fs/promises, node:readline): each
// signal holds the runtime's side of it (runtime/signal.c), which the
// compiler passes where a call takes a `signal`, and its abort runs the
// runtime's listeners in their places among those of the program.

// What this code asks of the runtime (runtime/library.c, signal.c): the
// name of the class of an object; the runtime's side of a new AbortSignal,
// that side aborted with the reason, the runtime's listeners that came
// before the listener numbered `order`, and the number of a new listener.
function __tz_lib_class_name(object: object): string
function __tz_signal_new(): object
function __tz_lib_signal_abort(native: object, reason: unknown): void
function __tz_lib_signal_run(native: object, order: double): void
function __tz_lib_order(): double

// A DOM exception: what AbortController.abort() gives as the reason, by
// default.
export class DOMException extends Error {
  // The legacy code of the name, as node's DOMException has it.
  @__tz_hidden readonly code: int
  constructor(message = "", name = "Error") {
    super(message)
    this.name = name
    this.code = legacyCode(name)
  }
}

function legacyCode(name: string): int {
  const names = [
    "IndexSizeError", "", "HierarchyRequestError", "WrongDocumentError", "InvalidCharacterError",
    "", "NoModificationAllowedError", "NotFoundError", "NotSupportedError", "InUseAttributeError",
    "InvalidStateError", "SyntaxError", "InvalidModificationError", "NamespaceError",
    "InvalidAccessError", "", "TypeMismatchError", "SecurityError", "NetworkError", "AbortError",
    "URLMismatchError", "QuotaExceededError", "TimeoutError", "InvalidNodeTypeError",
    "DataCloneError",
  ]
  const at = names.indexOf(name)
  return name == "" || at < 0 ? 0 : at + 1
}

export interface EventInit {
  bubbles?: boolean
  cancelable?: boolean
  composed?: boolean
}

export class Event {
  static readonly NONE = 0
  static readonly CAPTURING_PHASE = 1
  static readonly AT_TARGET = 2
  static readonly BUBBLING_PHASE = 3

  readonly type: string
  defaultPrevented = false
  readonly cancelable: boolean
  readonly timeStamp: double
  @__tz_hidden readonly bubbles: boolean
  @__tz_hidden readonly composed: boolean
  @__tz_hidden isTrusted = false
  // Set by the target that dispatches the event: what node's getters give.
  @__tz_hidden target: EventTarget | undefined = undefined
  @__tz_hidden currentTarget: EventTarget | undefined = undefined
  @__tz_hidden srcElement: EventTarget | undefined = undefined
  @__tz_hidden eventPhase = 0
  @__tz_hidden returnValue = true
  @__tz_hidden cancelBubble = false
  #dispatching = false
  #stopImmediate = false

  constructor(type: string, options?: EventInit) {
    this.type = type
    this.bubbles = options?.bubbles ?? false
    this.cancelable = options?.cancelable ?? false
    this.composed = options?.composed ?? false
    this.timeStamp = performance.now()
  }

  preventDefault() {
    if (!this.cancelable) return
    this.defaultPrevented = true
    this.returnValue = false
  }

  stopPropagation() {
    this.cancelBubble = true
  }

  stopImmediatePropagation() {
    this.cancelBubble = true
    this.#stopImmediate = true
  }

  composedPath(): EventTarget[] {
    const target = this.currentTarget
    return this.#dispatching && target != undefined ? [target] : []
  }

  // The steps of dispatchEvent that are the event's (see EventTarget).
  beginDispatch(target: EventTarget) {
    if (this.#dispatching)
      throw new EventRecursionError(`The event "${this.type}" is already being dispatched`)
    this.#dispatching = true
    this.target = target
    this.srcElement = target
    this.currentTarget = target
    this.eventPhase = Event.AT_TARGET
  }

  endDispatch() {
    this.#dispatching = false
    this.currentTarget = undefined
    this.eventPhase = Event.NONE
  }

  stoppedImmediately() {
    return this.#stopImmediate
  }
}

// node's ERR_EVENT_RECURSION.
class EventRecursionError extends Error {
  code = "ERR_EVENT_RECURSION"
}

// The options of `new CustomEvent<T>(type, options)`: the detail, a T, is
// not left out.
interface CustomEventInit<T> {
  detail: T
  bubbles?: boolean
  cancelable?: boolean
  composed?: boolean
}

export class CustomEvent<T> extends Event {
  readonly detail: T
  constructor(type: string, options: CustomEventInit<T>) {
    super(type, {
      bubbles: options.bubbles,
      cancelable: options.cancelable,
      composed: options.composed,
    })
    this.detail = options.detail
  }
}

export interface EventListenerObject {
  handleEvent(event: Event): void
}

export interface AddEventListenerOptions {
  once?: boolean
  capture?: boolean
  passive?: boolean
  signal?: AbortSignal
}

// A listener of an EventTarget, in the list of its type that node links:
// one that a listener removes while the event is dispatched is marked
// removed, and the dispatch, which holds the next one already, skips it.
// The list links only forward, so that it makes no cycle of references.
class Listener {
  next: Listener | undefined = undefined
  removed = false
  // Its place among the listeners of the runtime's own (see AbortSignal).
  readonly order = __tz_lib_order()
  constructor(
    readonly callback: (event: Event) => void,
    readonly original: unknown,
    readonly once: boolean,
    readonly capture: boolean,
  ) {}
}

export class EventTarget {
  // The head of the list of each type: a listener that is no listener.
  #events = new Map<string, Listener>()

  // addEventListener(type, listener, options) and removeEventListener() of
  // the program: the compiler calls these (src/sema/sema_events.cpp). A
  // function gets the kind of event E it takes, `(e: CustomEvent<int>) =>
  // ...`, and Event if it says none; the dispatch of another event to it is
  // a TypeError. An object gets the event in handleEvent().
  addFunctionListener<E extends Event>(
    type: string,
    listener: (event: E) => void,
    options?: AddEventListenerOptions,
  ) {
    const callback = (event: Event) => {
      const wanted = __tz_as<E>(event)
      if (wanted == undefined)
        throw new TypeError(
          `the listener of "${type}" takes another kind of event than '${__tz_lib_class_name(event)}'`,
        )
      listener(wanted)
    }
    this.#add(type, callback, listener, options)
  }

  addObjectListener(
    type: string,
    listener: EventListenerObject,
    options?: AddEventListenerOptions,
  ) {
    this.#add(type, (event: Event) => listener.handleEvent(event), listener, options)
  }

  #add(
    type: string,
    callback: (event: Event) => void,
    original: unknown,
    options?: AddEventListenerOptions,
  ) {
    const signal = options?.signal
    if (signal != undefined && signal.aborted) return
    const capture = options?.capture ?? false
    let root = this.#events.get(type)
    if (root == undefined) {
      root = new Listener(noListener, undefined, false, false)
      this.#events.set(type, root)
    }
    // The same listener with the same capture is there once.
    let last = root
    for (let at = root.next; at != undefined; at = at.next) {
      if (at.original == original && at.capture == capture) return
      last = at
    }
    last.next = new Listener(callback, original, options?.once ?? false, capture)
    if (signal != undefined) {
      const remove = (_event: Event) => this.removeListenerOf(type, original, capture)
      signal.addFunctionListener<Event>("abort", remove, { once: true })
    }
  }

  removeListenerWith(type: string, listener: unknown, options?: { capture?: boolean }) {
    this.removeListenerOf(type, listener, options?.capture ?? false)
  }

  removeListenerOf(type: string, original: unknown, capture: boolean) {
    const root = this.#events.get(type)
    if (root == undefined) return
    let previous = root
    for (let at = root.next; at != undefined; at = at.next) {
      if (at.original == original && at.capture == capture) {
        at.removed = true
        previous.next = at.next
        return
      }
      previous = at
    }
  }

  // The listeners of the runtime's own to events of `type` that came before
  // the listener numbered `order` (see AbortSignal); other targets have none.
  protected runtimeListeners(type: string, order: double) {
    void type
    void order
  }

  dispatchEvent(event: Event): boolean {
    event.beginDispatch(this)
    const root = this.#events.get(event.type)
    let handler = root?.next
    while (handler != undefined) {
      // Cache the next item in case this iteration removes the current one.
      const next: Listener | undefined = handler.next
      // Those of the runtime run whatever the program's do, as node's do
      // (kResistStopPropagation).
      this.runtimeListeners(event.type, handler.order)
      if (handler.removed || event.stoppedImmediately()) {
        handler = next
        continue
      }
      if (handler.once) this.removeListenerOf(event.type, handler.original, handler.capture)
      try {
        handler.callback(event)
      } catch (err) {
        // As node: the error of a listener is the program's, on the next
        // tick; the dispatch goes on.
        process.nextTick(() => {
          throw err
        })
      }
      handler = next
    }
    this.runtimeListeners(event.type, Infinity)
    event.endDispatch()
    return !event.defaultPrevented
  }

  // getEventListeners(target, type) of node:events.
  listenersOf(type: string): unknown[] {
    const out: unknown[] = []
    const root = this.#events.get(type)
    for (let at = root?.next; at != undefined; at = at.next) out.push(at.original)
    return out
  }
}

export class AbortSignal extends EventTarget {
  aborted = false
  @__tz_hidden reason: unknown = undefined
  // The handler of the abort event: a listener from when it is first set,
  // in its place among the others, as node's event handlers are (see
  // setOnabort).
  @__tz_hidden onabort: ((event: Event) => void) | undefined = undefined
  // The runtime's side (runtime/signal.c): what the runtime's waits listen
  // to, each with a number from the counter of the listeners.
  #native: object = __tz_signal_new()
  // The signals of any() that follow this one. They do not hold it, so a
  // signal and those that follow it make no cycle.
  #dependants: AbortSignal[] = []
  #onabortListens = false

  protected runtimeListeners(type: string, order: double) {
    if (type == "abort") __tz_lib_signal_run(this.#native, order)
  }

  // `signal.onabort = handler` of the program: the compiler calls this
  // (src/sema/sema_events.cpp). Set to undefined, the handler stays in its
  // place and does nothing.
  setOnabort(handler: ((event: Event) => void) | undefined) {
    this.onabort = handler
    if (this.#onabortListens) return
    this.#onabortListens = true
    this.addFunctionListener<Event>("abort", runOnabort)
  }

  throwIfAborted() {
    if (this.aborted) throw this.reason
  }

  // abortSignal() of node: this signal and the signals of any() that follow
  // it are aborted first, then the listeners of each run.
  abortWith(reason: unknown) {
    if (this.aborted) return
    const followers: AbortSignal[] = []
    this.#markAborted(reason, followers)
    this.#runAbort()
    for (const follower of followers) follower.#runAbort()
  }

  #markAborted(reason: unknown, followers: AbortSignal[]) {
    this.aborted = true
    this.reason = reason
    __tz_lib_signal_abort(this.#native, reason)
    const dependants = this.#dependants
    this.#dependants = []
    for (const signal of dependants)
      if (!signal.aborted) {
        followers.push(signal)
        signal.#markAborted(reason, followers)
      }
  }

  #runAbort() {
    const event = new Event("abort")
    event.isTrusted = true
    this.dispatchEvent(event)
  }

  static abort(reason?: unknown): AbortSignal {
    const signal = new AbortSignal()
    signal.#markAborted(reason ?? new DOMException("This operation was aborted", "AbortError"), [])
    return signal
  }

  // A signal that aborts after `delay` milliseconds, with a TimeoutError. Its
  // timer does not keep the program alive, as in node.
  static timeout(delay: double): AbortSignal {
    const signal = new AbortSignal()
    setTimeout(() => {
      signal.abortWith(new DOMException("The operation was aborted due to timeout", "TimeoutError"))
    }, delay).unref()
    return signal
  }

  // A signal that aborts when the first of `signals` does, with its reason.
  static any(signals: AbortSignal[]): AbortSignal {
    const result = new AbortSignal()
    for (const signal of signals)
      if (signal.aborted) {
        result.#markAborted(signal.reason, [])
        return result
      }
    for (const signal of signals)
      if (!signal.#dependants.includes(result)) signal.#dependants.push(result)
    return result
  }
}

export class AbortController {
  readonly signal = new AbortSignal()

  abort(reason?: unknown) {
    this.signal.abortWith(reason ?? new DOMException("This operation was aborted", "AbortError"))
  }
}

// The listener that `onabort` of a signal is: it calls what the field holds
// when the signal aborts.
function runOnabort(event: Event) {
  const signal = event.currentTarget
  if (!(signal instanceof AbortSignal)) return
  const handler = signal.onabort
  if (handler != undefined) handler(event)
}

// The callback of the head of a list of listeners, which never runs.
function noListener(_event: Event) {}
