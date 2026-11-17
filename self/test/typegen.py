"""Enumerates tz types together with the trees they must give and checks the
self/ parser against them.  Works on top of exprgen.py: same runner, same
report.

    python typegen.py <tzparse.exe> <work dir> [levels|all]

levels: A types without parts, R1 every construct with plain parts, L1 every
plain type in every place, L2 every construct in every place of another,
S2 two constructs side by side in one, H the types of A, R1, L1 and L2 in
every place of the language that takes a type (a variable, a parameter, a
result, 'as', type arguments, a limit of a type parameter), L3 three
constructs deep (only with 'all').

The text is printed with the fewest parentheses the grammar of TypeScript
types allows.  How a statement around a type looks in the tree is taken
from the parser once, with a plain name in the place of the type.

Where tz knowingly differs from TypeScript the generator follows tz:
  - no constructor types ('new () => A'), no 'unique symbol', no generic
    function types ('<U>(p: U) => U'), no optional elements of a tuple
    ('[A?]'), no call signatures in an object type.

As in TypeScript, the result type of an arrow function cannot end with a
type in parentheses: '(): (A) => a' reads '(A) => a' as a function type.
Such lines are not generated.
"""

import collections
import os
import subprocess
import sys

import exprgen as E
from exprgen import S

(T_TYPE, T_UNION, T_INTERSECTION, T_OPERATOR, T_POSTFIX,
 T_PRIMARY) = range(1, 7)


class Ty:
    __slots__ = ("text", "prec", "shape", "kind", "desc", "kids", "fails",
                 "line", "block", "function")

    def __init__(self, text, prec, shape, kind, function=False):
        self.text = text
        self.prec = prec
        self.shape = shape
        self.kind = kind
        self.desc = kind
        self.kids = ()
        self.fails = None
        self.line = None
        self.block = None
        self.function = function


def T(kind, word, kids=(), extras=""):
    label = 'Type %s "%s" n=%d%s' % (kind, word, len(kids), extras)
    return S(label, *kids)


def wrap(part, least):
    if part.prec < least:
        return "(" + part.text + ")"
    return part.text


def named(word):
    return Ty(word, T_PRIMARY, T("Name", word), word)


def literal(text, word, written, number, integer, beyond=0):
    extras = ' text="%s" number=%s int=%d beyond=%d' % (
        written, E.bits(number), integer, beyond)
    return Ty(text, T_PRIMARY, T("Literal", word, (), extras), text)


def function_shape(names, optional, rest, kids):
    listed = ",".join('"%s"' % word for word in names)
    flags = "".join("1" if flag else "0" for flag in optional) or "-"
    extras = " names=[%s] opt=%s rest=%d" % (listed, flags, rest)
    return T("Function", "", kids, extras)


def object_shape(names, optional, readonly, methods, kids):
    def flags(values):
        return "".join("1" if value else "0" for value in values) or "-"

    last = len(methods)
    while last > 0 and not methods[last - 1]:
        last -= 1
    listed = ",".join('"%s"' % word for word in names)
    extras = " names=[%s] opt=%s ro=%s methods=%s" % (
        listed, flags(optional), flags(readonly), flags(methods[:last]))
    return T("Object", "", kids, extras)


def tuple_shape(spread, kids):
    flags = "".join("1" if flag else "0" for flag in spread) or "-"
    zeros = "0" * len(kids) or "-"
    return T("Tuple", "", kids, " ro=%s spread=%s" % (zeros, flags))


def atoms():
    plain_function = function_shape([], [], 0, [T("Name", "A")])
    return [
        named("int"),
        named("a.B"),
        named("undefined"),
        named("void"),
        named("never"),
        named("this"),
        literal('"s"', "string", "s", 0.0, 0),
        literal("1", "number", "", 1.0, 1),
        literal("-1", "number", "", -1.0, 1),
        literal("1.5", "number", "", 1.5, 0),
        literal("1e400", "number", "", 0.0, 0, beyond=1),
        literal("true", "boolean", "", 1.0, 0),
        literal("false", "boolean", "", 0.0, 0),
        literal("`t`", "string", "t", 0.0, 0),
        Ty("{ }", T_PRIMARY, object_shape([], [], [], [], []), "{ }"),
        Ty("[]", T_PRIMARY, tuple_shape([], []), "[]"),
        Ty("typeof a", T_PRIMARY, T("TypeOf", "", (), ' names=["a"]'),
           "typeof"),
        Ty("typeof a.b", T_PRIMARY, T("TypeOf", "", (), ' names=["a","b"]'),
           "typeof a.b"),
        Ty("() => A", T_TYPE, plain_function, "()=>A", function=True),
    ]


# ------------------------------------------------------------ constructs

def array(item):
    return Ty(wrap(item, T_POSTFIX) + "[]", T_POSTFIX,
              T("Array", "", [item.shape]), "[]")


def readonly_array(item):
    return Ty("readonly " + wrap(item, T_POSTFIX) + "[]", T_OPERATOR,
              T("Array", "", [item.shape]), "readonly[]")


def indexed(target, key):
    text = "%s[%s]" % (wrap(target, T_POSTFIX), key.text)
    return Ty(text, T_POSTFIX, T("Indexed", "", [target.shape, key.shape]),
              "[k]")


def joined(sign, kind, least, leading=False):
    def build(*parts):
        text = (" %s " % sign).join(wrap(part, least) for part in parts)
        if leading:
            text = sign + " " + text
        prec = T_UNION if sign == "|" else T_INTERSECTION
        return Ty(text, prec, T(kind, "", [part.shape for part in parts]),
                  sign)
    return build


def key_of(target):
    return Ty("keyof " + wrap(target, T_OPERATOR), T_OPERATOR,
              T("KeyOf", "", [target.shape]), "keyof")


def generic(word):
    def build(*args):
        text = "%s<%s>" % (word, ", ".join(arg.text for arg in args))
        return Ty(text, T_PRIMARY,
                  T("Generic", word, [arg.shape for arg in args]), word)
    return build


def function(head, names, optional, rest):
    def build(*parts):
        text = head % tuple(part.text for part in parts)
        shape = function_shape(names, optional, rest,
                               [part.shape for part in parts])
        return Ty(text, T_TYPE, shape, "=>", function=True)
    return build


def predicate(param, target):
    text = "(p: %s) => p is %s" % (param.text, target.text)
    result = T("Predicate", "p", [target.shape], " asserts=0")
    shape = function_shape(["p"], [False], 0, [param.shape, result])
    return Ty(text, T_TYPE, shape, "is", function=True)


def object_(text, names, optional, readonly, methods=None):
    def build(*parts):
        marks = methods or [False] * len(names)
        shape = object_shape(names, optional, readonly, marks,
                             [part.shape for part in parts])
        return Ty(text % tuple(part.text for part in parts), T_PRIMARY, shape,
                  "{}")
    return build


def method(param, result):
    inner = function_shape(["p"], [False], 0, [param.shape, result.shape])
    shape = object_shape(["m"], [False], [True], [True], [inner])
    text = "{ m(p: %s): %s }" % (param.text, result.text)
    return Ty(text, T_PRIMARY, shape, "{m()}")


def record(value):
    shape = T("Generic", "Record", [T("Name", "string"), value.shape])
    return Ty("{ [k: string]: %s }" % value.text, T_PRIMARY, shape, "{[k]}")


def tuple_(text, spread):
    def build(*parts):
        return Ty(text % tuple(part.text for part in parts), T_PRIMARY,
                  tuple_shape(spread, [part.shape for part in parts]), "[,]")
    return build


def mapped(text, optional, readonly):
    def build(*parts):
        extras = " opt=%d ro=%d" % (optional, readonly)
        shape = T("Mapped", "K", [part.shape for part in parts], extras)
        return Ty(text % tuple(part.text for part in parts), T_PRIMARY, shape,
                  "{[K in]}")
    return build


def mapped_as(keys, renamed, value):
    text = "{ [K in %s as %s]: %s }" % (keys.text, renamed.text, value.text)
    shape = T("Mapped", "K", [keys.shape, value.shape, renamed.shape],
              " opt=0 ro=0")
    return Ty(text, T_PRIMARY, shape, "{[K in as]}")


def limit(part):
    """The type after 'extends' of a conditional type: neither another
    conditional type nor a function type without parentheses."""
    return wrap(part, T_UNION)


def conditional(check, limit_, then, other):
    text = "%s extends %s ? %s : %s" % (wrap(check, T_UNION), limit(limit_),
                                        then.text, other.text)
    shape = T("Conditional", "", [check.shape, limit_.shape, then.shape,
                                  other.shape])
    return Ty(text, T_TYPE, shape, "?:")


def conditional_infer(check, other):
    text = "%s extends (infer U)[] ? U : %s" % (wrap(check, T_UNION),
                                                other.text)
    shape = T("Conditional", "", [
        check.shape, T("Array", "", [T("Infer", "U")]), T("Name", "U"),
        other.shape])
    return Ty(text, T_TYPE, shape, "infer")


def template(part):
    shape = T("Template", "", [part.shape], ' texts=["a","b"]')
    return Ty("`a${" + part.text + "}b`", T_PRIMARY, shape, "`${}`")


def paren(part):
    return Ty("(" + part.text + ")", T_PRIMARY, part.shape, "paren")


Construct = collections.namedtuple("Construct", "name slots build")


def constructs():
    made = []

    def add(label, slots, build):
        made.append(Construct(label, slots, build))

    add("[]", 1, array)
    add("readonly[]", 1, readonly_array)
    add("[k]", 2, indexed)
    add("|", 2, joined("|", "Union", T_INTERSECTION))
    add("||", 3, joined("|", "Union", T_INTERSECTION))
    add("lead|", 2, joined("|", "Union", T_INTERSECTION, leading=True))
    add("&", 2, joined("&", "Intersection", T_OPERATOR))
    add("keyof", 1, key_of)
    add("A<>", 1, generic("A"))
    add("A<,>", 2, generic("A"))
    add("a.B<>", 1, generic("a.B"))
    add("()=>", 1, function("() => %s", [], [], 0))
    add("(p)=>", 2, function("(p: %s) => %s", ["p"], [False], 0))
    add("(p,q?)=>", 3,
        function("(p: %s, q?: %s) => %s", ["p", "q"], [False, True], 0))
    add("(...p)=>", 2, function("(...p: %s) => %s", ["p"], [False], 1))
    add("is", 2, predicate)
    add("{a}", 1, object_("{ a: %s }", ["a"], [False], [False]))
    add("{a;b?;ro c}", 3,
        object_("{ a: %s; b?: %s; readonly c: %s }", ["a", "b", "c"],
                [False, True, False], [False, False, True]))
    add("{a,b}", 2,
        object_("{ a: %s, b: %s }", ["a", "b"], [False, False],
                [False, False]))
    add("{a;}", 1, object_("{ a: %s; }", ["a"], [False], [False]))
    add("{m()}", 2, method)
    add("{[k]}", 1, record)
    add("[x]", 1, tuple_("[%s]", [False]))
    add("[x,y]", 2, tuple_("[%s, %s]", [False, False]))
    add("[x,...y]", 2, tuple_("[%s, ...%s]", [False, True]))
    add("[a:x,b:y]", 2, tuple_("[a: %s, b: %s]", [False, False]))
    add("[x,]", 1, tuple_("[%s,]", [False]))
    add("{[K in]}", 2, mapped("{ [K in %s]: %s }", 0, 0))
    add("{[K in]?}", 2, mapped("{ [K in %s]?: %s }", 1, 0))
    add("{ro [K in]}", 2, mapped("{ readonly [K in %s]: %s }", 0, 1))
    add("{-ro [K in]-?}", 2, mapped("{ -readonly [K in %s]-?: %s }", -1, -1))
    add("{[K in as]}", 3, mapped_as)
    add("?:", 4, conditional)
    add("infer", 2, conditional_infer)
    add("`${}`", 1, template)
    add("paren", 1, paren)
    return made


def fill(construct, deep, words):
    kids = []
    for at in range(construct.slots):
        given = deep.get(at)
        kids.append(named(words[at]) if given is None else given)
    made = construct.build(*kids)
    made.kind = construct.name
    made.kids = tuple(kids)
    places = ",".join("%d=%s" % (at, deep[at].desc) for at in sorted(deep))
    made.desc = construct.name + ("[" + places + "]" if places else "")
    return made


# -------------------------------------------------- the places of a type

ALIAS = "type T = %s;"

HOSTS = [
    ("let", "let v: %s;"),
    ("const", "const v: %s = a;"),
    ("parameter", "function f(p: %s) { }"),
    ("parameter=", "function f(p: %s = a) { }"),
    ("result", "function f(): %s { }"),
    ("arrow parameter", "const v = (p: %s) => a;"),
    ("arrow result", "const v = (): %s => a;"),
    ("as", "const v = a as %s;"),
    ("call<>", "const v = f<%s>(a);"),
    ("new<>", "const v = new A<%s>(a);"),
    ("limit", "function f<U extends %s>() { }"),
    ("default", "function f<U = %s>() { }"),
    ("guard", "function f(p: A): p is %s { }"),
    ("alias limit", "type T<U extends %s> = U;"),
]

SENTINEL = "Zz"
SENTINEL_LINE = 'Type Name "%s" n=0' % SENTINEL


class Frame:
    """The lines of the tree around the type, learnt from the parser."""

    def __init__(self, before, depth, after):
        self.before = before
        self.depth = depth
        self.after = after

    def block(self, shape):
        out = list(self.before)
        E.flatten(shape, self.depth, out)
        out.extend(self.after)
        return "\n".join(out)


def learn_frames(exe, work):
    patterns = [("alias", ALIAS)] + HOSTS
    path = os.path.join(work, "frames.tz")
    with open(path, "w", newline="\n") as out:
        for _, pattern in patterns:
            out.write(pattern % SENTINEL + "\n")
    dumped = subprocess.run([exe, "tree", path], capture_output=True,
                            text=True, check=False).stdout.split("\n")
    os.remove(path)
    blocks = []
    for line in dumped[2:]:
        if line.startswith("imports "):
            break
        if line[:1] != " ":
            blocks.append([])
        blocks[-1].append(E.normalize(line))
    assert len(blocks) == len(patterns), (len(blocks), len(patterns))
    assert "!! " not in "\n".join(dumped), "a frame does not parse"
    frames = {}
    for (label, _), lines in zip(patterns, blocks):
        places = [at for at, line in enumerate(lines)
                  if line.strip() == SENTINEL_LINE]
        assert len(places) == 1, (label, lines)
        at = places[0]
        depth = (len(lines[at]) - len(lines[at].lstrip())) // 2
        frames[label] = Frame(lines[:at], depth, lines[at + 1:])
    return frames


class Line:
    """A type in one of its places: a whole line of source."""
    __slots__ = ("text", "kind", "desc", "fails", "line", "block")

    def __init__(self, text, kind, desc, block):
        self.text = text
        self.kind = kind
        self.desc = desc
        self.fails = None
        self.line = text + "\n"
        self.block = block


def placed(frames, label, pattern, part):
    text = part.text
    if label == "arrow result" and text.endswith(")"):
        return None
    desc = part.desc if label == "alias" else "%s: %s" % (label, part.desc)
    return Line(pattern % text, part.kind, desc,
                frames[label].block(part.shape))


# ----------------------------------------------------------- enumeration

def one_deep(made, inner, words, level):
    for construct in made:
        for at in range(construct.slots):
            for item in inner:
                part = fill(construct, {at: item.node}, words)
                pair = (construct.name, at, item.node.kind)
                yield E.Item(level, (pair,) + item.pairs, part)


def two_deep(made, inner, words):
    for construct in made:
        for first in range(construct.slots):
            for second in range(first + 1, construct.slots):
                for left in inner:
                    for right in inner:
                        part = fill(construct, {first: left.node,
                                                second: right.node}, words)
                        pairs = ((construct.name, first, left.node.kind),
                                 (construct.name, second, right.node.kind))
                        yield E.Item("S2", pairs, part)


def in_alias(frames, items):
    for item in items:
        yield E.Item(item.level, item.pairs,
                     placed(frames, "alias", ALIAS, item.node))


def in_hosts(frames, items):
    for label, pattern in HOSTS:
        for item in items:
            pair = (label, 0, item.node.kind)
            line = placed(frames, label, pattern, item.node)
            if line is not None:
                yield E.Item("H", (pair,) + item.pairs, line)


def enumerate_levels(levels):
    frames = learn_frames(sys.argv[1], sys.argv[2])
    made = constructs()
    alone = [E.Item("A", (), part) for part in atoms()]
    alone.append(E.Item("A", (), named("A")))
    plain = [E.Item("R1", (), fill(construct, {}, "ABCD"))
             for construct in made]
    with_atoms = list(one_deep(made, alone, "ABCD", "L1"))
    pairs = list(one_deep(made, plain, "WXYZ", "L2"))
    if "A" in levels:
        yield from in_alias(frames, alone)
    if "R1" in levels:
        yield from in_alias(frames, plain)
    if "L1" in levels:
        yield from in_alias(frames, with_atoms)
    if "L2" in levels:
        yield from in_alias(frames, pairs)
    if "S2" in levels:
        yield from in_alias(frames, two_deep(made, plain, "WXYZ"))
    if "H" in levels:
        yield from in_hosts(frames, alone + plain + with_atoms + pairs)
    if "L3" in levels:
        yield from in_alias(frames, one_deep(made, pairs, "STUV", "L3"))


if __name__ == "__main__":
    os.makedirs(sys.argv[2], exist_ok=True)
    E.main(enumerate_levels, ["A", "R1", "L1", "L2", "S2", "H"], "L3")
