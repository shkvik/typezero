"""Enumerates tz declarations and module statements together with the trees
they must give and checks the self/ parser against them.  Works on top of
exprgen.py (the runner), typegen.py and stmtgen.py (the parts).

    python declgen.py <tzparse.exe> <work dir> [levels]

levels: A imports, exports and declarations without parts, R1 every
declaration with plain parts, X 'export' before every declaration, T1 every
plain type in every place of a type, E1 every plain expression in every
place of an expression, S1 every statement in every body, P2 two members of
a class side by side, M1 expressions the parser must refuse, T2 and E2 the
pairs of types and of expressions in those places.

Where tz knowingly differs from TypeScript the generator follows tz:
  - no getters and setters, no static blocks, no member names in brackets,
    no 'declare' fields, no optional methods, no overload signatures over
    one body, no generic methods in an interface;
  - the fields a constructor declares in its parameters ('private p: T')
    come first among the fields of the class, wherever the constructor is;
  - a part of a class ('partial class') is not exported;
  - imports are by name: no 'import * as', no default import (but a JSON
    file is imported whole), no 'import type'; no 'export default', no
    'export * as'.
"""

import collections
import os
import re
import sys

import exprgen as E
import stmtgen as St
import typegen as Ty
from exprgen import ASSIGN, S

MISSING = E.MISSING
INT = E.INT_TYPE
LOOP_EXITS = St.LOOP_EXITS


class Decl:
    __slots__ = ("text", "shape", "kind", "desc", "fails", "kids", "line",
                 "block", "declares", "extra")

    def __init__(self, text, shape, kind, declares=(), extra=()):
        self.text = text
        self.shape = shape
        self.kind = kind
        self.desc = kind
        self.fails = None
        self.kids = ()
        self.declares = tuple(declares)
        self.extra = tuple(extra)
        self.line = text + "\n"
        self.block = "\n".join([St.lines_of(shape)] + list(extra))


def name_shape(word):
    return S('Name "%s" targs=0' % word)


def value(expr):
    """An expression where neither 'yield' nor 'await' can be."""
    if St.needs(expr):
        return None
    return E.wrap(expr, ASSIGN)[0]


def body_fits(item, generator=False, is_async=False):
    free = St.needs(item)
    if free & LOOP_EXITS:
        return False
    if "yield" in free and not generator:
        return False
    return "await" not in free or is_async


# ------------------------------------------------------------------ enums

def enum(text, members):
    listed = [S('EnumMember "%s"' % word, init) for word, init in members]
    return Decl(text, S('Enum "E" n=%d' % len(listed), *listed), "enum",
                ["E"])


def enum_one(init):
    text = value(init)
    if text is None:
        return None
    return enum("enum E { A = %s }" % text, [("A", init.shape)])


def enum_two(first, second):
    texts = (value(first), value(second))
    if None in texts:
        return None
    return enum("enum E { A = %s, B = %s }" % texts,
                [("A", first.shape), ("B", second.shape)])


def enum_trailing(init):
    text = value(init)
    if text is None:
        return None
    return enum("enum E { A, B = %s, }" % text,
                [("A", MISSING), ("B", init.shape)])


# ------------------------------------------------------- types with names

def type_param(word, limit=MISSING, default=MISSING):
    return S('TypeParam "%s"' % word, limit, default)


def alias(word, params, shape, bases=()):
    line = 'TypeAlias "%s" tp=%d bases=%d' % (word, len(params),
                                                      len(bases))
    return S(line, *params, shape, *bases)


EMPTY_OBJECT = Ty.object_shape([], [], [], [], [])
OBJECT_KINDS = ("{a}", "{a;b?;ro c}", "{a,b}", "{a;}", "{m()}")


def interface_of(construct):
    def build(*parts):
        body = construct.build(*parts)
        return Decl("interface I " + body.text, alias("I", [], body.shape),
                    "interface", ["I"])
    return build


def interface_extends(*bases):
    text = "interface I extends %s { }" % ", ".join(b.text for b in bases)
    shape = alias("I", [], EMPTY_OBJECT, [base.shape for base in bases])
    return Decl(text, shape, "interface", ["I"])


def interface_generic(limit, default):
    text = "interface I<T extends %s = %s> { }" % (limit.text, default.text)
    shape = alias("I", [type_param("T", limit.shape, default.shape)],
                  EMPTY_OBJECT)
    return Decl(text, shape, "interface", ["I"])


def alias_generic(target):
    shape = alias("T", [type_param("U")], target.shape)
    return Decl("type T<U> = %s;" % target.text, shape, "type", ["T"])


def alias_limited(limit, default, target):
    text = "type T<U extends %s = %s> = %s;" % (limit.text, default.text,
                                                target.text)
    shape = alias("T", [type_param("U", limit.shape, default.shape)],
                  target.shape)
    return Decl(text, shape, "type", ["T"])


# -------------------------------------------------------------- functions

def function_line(word, generator, is_async, body, type_params, params):
    return ('Function "%s" arrow=0 concise=0 own="" place=0 body=%d gen=%d '
            'async=%d role=plain tp=%d params=%d'
            % (word, body, generator, is_async, type_params, params))


def parameter(word, form, type_, default=None):
    kids = [type_] if default is None else [type_, default]
    return S('Param "%s" form=%d prop=- dec=0' % (word, form), *kids)


def function_generic(item):
    if not body_fits(item):
        return None
    t_type = Ty.T("Name", "T")
    shape = S(function_line("f", 0, 0, 0, 1, 1), type_param("T"),
              parameter("p", 0, t_type), t_type, S("Block n=1", item.shape))
    text = "function f<T>(p: T): T { %s }" % item.text
    return Decl(text, shape, "function", ["f"])


def function_typed(first, default, second, rest, result, item):
    written = value(default)
    if written is None or not body_fits(item):
        return None
    text = "function f(p: %s = %s, q?: %s, ...r: %s): %s { %s }" % (
        first.text, written, second.text, rest.text, result.text, item.text)
    shape = S(function_line("f", 0, 0, 0, 0, 3),
              parameter("p", 2, first.shape, default.shape),
              parameter("q", 1, second.shape),
              parameter("r", 3, rest.shape), result.shape,
              S("Block n=1", item.shape))
    return Decl(text, shape, "function", ["f"])


def function_declared(param, result):
    text = "declare function f(p: %s): %s;" % (param.text, result.text)
    shape = S(function_line("f", 0, 0, 3, 0, 1),
              parameter("p", 0, param.shape), result.shape, S("Block n=0"))
    return Decl(text, shape, "declare function", ["f"])


def function_this(owner):
    text = "function f(this: %s) { }" % owner.text
    shape = S(function_line("f", 0, 0, 0, 0, 1),
              parameter("this", 0, owner.shape), MISSING, S("Block n=0"))
    return Decl(text, shape, "function", ["f"])


def function_patterns(first, second, item):
    if not body_fits(item):
        return None
    text = "function f({ v, w }: %s, [x, y]: %s) { %s }" % (
        first.text, second.text, item.text)
    parts = S("VarDecl const=0 using=0 n=4",
              St.declarator("v", St.field("v")),
              St.declarator("w", St.field("w")),
              St.declarator("x", St.element(0)),
              St.declarator("y", St.element(1)))
    shape = S(function_line("f", 0, 0, 0, 0, 2),
              parameter("%", 0, first.shape), parameter("%", 0, second.shape),
              MISSING, S("Block n=2", parts, item.shape))
    return Decl(text, shape, "function", ["f"])


# ---------------------------------------------------------------- classes

Member = collections.namedtuple("Member", "text fields methods ctors")

DECORATOR_NAME = re.compile(r"[A-Za-z_]\w*(\.[A-Za-z_]\w*)*")
NOT_A_DECORATOR = ("this", "true", "false", "undefined")


def decorator(expr):
    """The text and the tree of '@expr': a name or its members as they are,
    anything else in parentheses."""
    text = value(expr)
    if text is None:
        return None
    first = expr.text.split(".")[0]
    if not DECORATOR_NAME.fullmatch(expr.text) or first in NOT_A_DECORATOR:
        text = "(" + expr.text + ")"
    return "@" + text, S("Decorator", expr.shape)


def field_shape(word, form=0, presence=0, readonly=0, access=0, type_=MISSING,
                init=MISSING, decorators=()):
    line = ('Field "%s" form=%d presence=%d ro=%d override=0 access=%d '
            'accessor=0 dec=%d' % (word, form, presence, readonly, access,
                                   len(decorators)))
    return S(line, *decorators, type_, init)


def method_shape(word, body, params=(), result=MISSING, generator=0,
                 is_async=0, access=0, override=0, role=0, decorators=()):
    line = ('Function "%s" arrow=0 concise=0 own="" place=0 body=0 gen=%d '
            'async=%d role=member %d %d %d %d tp=0 params=%d'
            % (word, generator, is_async, access, override, role,
               len(decorators), len(params)))
    return S(line, *decorators, *params, result, body)


def one_field(text, **marks):
    def build(*parts):
        types = [part for part in parts if isinstance(part, Ty.Ty)]
        inits = [part for part in parts if isinstance(part, E.Node)]
        written = [part.text for part in types]
        for init in inits:
            init_text = value(init)
            if init_text is None:
                return None
            written.append(init_text)
        type_ = types[0].shape if types else marks.get("type_", MISSING)
        init = inits[0].shape if inits else MISSING
        shape = field_shape(marks.get("word", "x"), marks.get("form", 0),
                            marks.get("presence", 0), marks.get("readonly", 0),
                            marks.get("access", 0), type_, init)
        return Member(text % tuple(written), [shape], [], [])
    return build


def one_method(text, generator=0, is_async=0, **marks):
    def build(*parts):
        item = parts[-1]
        if not body_fits(item, generator, is_async):
            return None
        types = parts[:-1]
        params = []
        result = MISSING
        if len(types) == 2:
            params = [parameter("p", 0, types[0].shape)]
            result = types[1].shape
        elif len(types) == 1:
            result = types[0].shape
        shape = method_shape("m", S("Block n=1", item.shape), params, result,
                             generator, is_async, **marks)
        written = tuple(part.text for part in parts)
        return Member(text % written, [], [shape], [])
    return build


def constructor(item):
    if not body_fits(item):
        return None
    shape = method_shape("constructor", S("Block n=1", item.shape), role=2)
    return Member("constructor() { %s }" % item.text, [], [], [shape])


def constructor_typed(param, item):
    if not body_fits(item):
        return None
    shape = method_shape("constructor", S("Block n=1", item.shape),
                         [parameter("p", 0, param.shape)], role=2)
    text = "constructor(p: %s) { %s }" % (param.text, item.text)
    return Member(text, [], [], [shape])


def kept(word):
    target = S('Member "%s" opt=0 targs=0' % word, name_shape("this"))
    return S("ExprStmt", S("Assign -", target, name_shape(word)))


def constructor_fields(first, second, item):
    if not body_fits(item):
        return None
    text = ("constructor(private p: %s, public readonly q: %s) { %s }"
            % (first.text, second.text, item.text))
    fields = [field_shape("p", form=3, access=2, type_=first.shape),
              field_shape("q", form=3, readonly=1, type_=second.shape)]
    params = [S('Param "p" form=0 prop=02 dec=0', first.shape),
              S('Param "q" form=0 prop=10 dec=0', second.shape)]
    body = S("Block n=3", kept("p"), kept("q"), item.shape)
    shape = method_shape("constructor", body, params, role=2)
    return Member(text, fields, [], [shape])


def decorated_field(expr):
    made = decorator(expr)
    if made is None:
        return None
    shape = field_shape("x", type_=INT, decorators=[made[1]])
    return Member(made[0] + " x: int;", [shape], [], [])


def decorated_method(expr):
    made = decorator(expr)
    if made is None:
        return None
    shape = method_shape("m", S("Block n=0"), decorators=[made[1]])
    return Member(made[0] + " m() { }", [], [shape], [])


def decorated_parameter(expr):
    made = decorator(expr)
    if made is None:
        return None
    param = S('Param "p" form=0 prop=- dec=1', made[1], INT)
    shape = method_shape("m", S("Block n=0"), [param])
    return Member("m(%s p: int) { }" % made[0], [], [shape], [])


def members():
    made = []

    def add(label, slots, build):
        made.append(E.Construct(label, slots, build))

    add("x: T", ["type"], one_field("x: %s;"))
    add("x = E", ["expr"], one_field("x = %s;"))
    add("x: T = E", ["type", "expr"], one_field("x: %s = %s;"))
    add("x?: T", ["type"], one_field("x?: %s;", presence=1))
    add('"k": T', ["type"], one_field('"k": %s;', word="k"))
    add("public x", [], one_field("public x: int;", type_=INT))
    add("private x", [], one_field("private x: int;", type_=INT, access=2))
    add("protected x", [],
        one_field("protected x: int;", type_=INT, access=1))
    add("readonly x", [], one_field("readonly x: int;", type_=INT,
                                    readonly=1))
    add("static x", [], one_field("static x: int;", type_=INT, form=2))
    add("private static readonly x = E", ["expr"],
        one_field("private static readonly x: int = %s;", type_=INT, form=2,
                  readonly=1, access=2))
    add("m()", ["item"], one_method("m() { %s }"))
    add("m(p: T): U", ["type", "type", "item"],
        one_method("m(p: %s): %s { %s }"))
    add("private m()", ["item"], one_method("private m() { %s }", access=2))
    add("protected m()", ["item"],
        one_method("protected m() { %s }", access=1))
    add("static m()", ["item"], one_method("static m() { %s }", role=1))
    add("async m()", ["item"], one_method("async m() { %s }", is_async=1))
    add("*m()", ["item"], one_method("*m() { %s }", generator=1))
    add("static async *m()", ["item"],
        one_method("static async *m() { %s }", 1, 1, role=1))
    add("override m()", ["item"], one_method("override m() { %s }",
                                             override=1))
    add("public override async m(): T", ["type", "item"],
        one_method("public override async m(): %s { %s }", is_async=1,
                   override=1))
    add("constructor()", ["item"], constructor)
    add("constructor(p: T)", ["type", "item"], constructor_typed)
    add("constructor(private p)", ["type", "type", "item"],
        constructor_fields)
    add("@D x", ["expr"], decorated_field)
    add("@D m()", ["expr"], decorated_method)
    add("m(@D p)", ["expr"], decorated_parameter)
    return made


def class_shape(parts, word="A", abstract=0, partial=0, type_params=(),
                base=None, base_expr=None, interfaces=(), decorators=(),
                base_args=()):
    fields = [shape for part in parts for shape in part.fields]
    fields.sort(key=lambda shape: "form=3" not in shape[0])
    methods = [shape for part in parts for shape in part.methods]
    ctors = [shape for part in parts for shape in part.ctors]
    line = ('Class "%s" abstract=%d partial=%d expr=0 generic=%d tp=%d '
            'base=%d baseExpr=%d baseArgs=%d ifaces=%d dec=%d fields=%d '
            'methods=%d ctors=%d' % (
                word, abstract, partial, bool(type_params), len(type_params),
                base is not None, base_expr is not None, len(base_args),
                len(interfaces),
                len(decorators), len(fields), len(methods), len(ctors)))
    kids = list(decorators) + list(type_params)
    kids += [shape for shape in (base, base_expr) if shape is not None]
    kids += list(base_args)
    kids += list(interfaces) + fields + methods + ctors
    return S(line, *kids)


def class_with(parts, head="class A", **marks):
    written = " ".join(part.text for part in parts)
    text = "%s { %s }" % (head, written) if parts else head + " { }"
    return Decl(text, class_shape(parts, **marks), "class", ["A"])


def class_of(member):
    def build(*parts):
        part = member.build(*parts)
        if part is None:
            return None
        return class_with([part])
    return build


def class_extends_type(base):
    if "." in base.text.split("<")[0]:
        return None
    return class_with([], "class A extends " + base.text, base=base.shape)


def class_extends_value(base):
    probe = E.class_extends(base)
    if probe is None or St.needs(base):
        return None
    head = "class A extends " + E.wrap(base, E.CALL)[0]
    return class_with([], head, base_expr=base.shape)


def class_extends_typed_value(base):
    probe = E.class_extends(base)
    if probe is None or St.needs(base):
        return None
    head = "class A extends " + E.wrap(base, E.CALL)[0] + "<int, B>"
    return class_with([], head, base_expr=base.shape,
                      base_args=[INT, E.S('Type Name "B" n=0')])


def class_implements(*listed):
    head = "class A implements " + ", ".join(part.text for part in listed)
    return class_with([], head, interfaces=[part.shape for part in listed])


def class_generic(limit, default):
    head = "class A<T extends %s = %s>" % (limit.text, default.text)
    return class_with(
        [], head, type_params=[type_param("T", limit.shape, default.shape)])


def class_decorated(expr):
    made = decorator(expr)
    if made is None:
        return None
    return class_with([], made[0] + " class A", decorators=[made[1]])


def class_abstract_method(result):
    shape = method_shape("m", S("Block n=0"), result=result.shape, role=3)
    part = Member("abstract m(): %s;" % result.text, [], [shape], [])
    return class_with([part], "abstract class A", abstract=1)


# ---------------------------------------------------------------- modules

def module_line(text, *entries):
    return Decl(text, S("Empty"), "module", extra=entries)


def imported(path, *names, part_list=0):
    lines = ['Import "%s" "" 0 %d %d' % (path, part_list, len(names))]
    lines += ['  Name "%s" "%s"' % pair for pair in names]
    return lines


def exported(form, path, *names):
    lines = ['Export %d "%s" %d' % (form, path, len(names))]
    lines += ['  Name "%s" "%s"' % pair for pair in names]
    return lines


def module_atoms():
    same = ("a", "a")
    return [
        module_line('import { a } from "./m.tz";', *imported("./m.tz", same)),
        module_line('import { a, b as c } from "./m.tz";',
                    *imported("./m.tz", same, ("b", "c"))),
        module_line('import { a, } from "node:fs";',
                    *imported("node:fs", same)),
        module_line('import "./part.tz";',
                    *imported("./part.tz", part_list=1)),
        module_line('import config from "./config.json" with { type: "json" };',
                    *imported("./config.json", ("json", "config"))),
        module_line("export { a };", *exported(0, "", same)),
        module_line("export { a, b as c };",
                    *exported(0, "", same, ("b", "c"))),
        module_line("export { };", *exported(0, "")),
        module_line('export { a } from "./m.tz";',
                    *exported(1, "./m.tz", same)),
        module_line('export { a as b, c } from "./m.tz";',
                    *exported(1, "./m.tz", ("a", "b"), ("c", "c"))),
        module_line('export * from "./m.tz";', *exported(2, "./m.tz")),
    ]


VARIABLE_NAMES = {
    "const": ["v"], "let": ["v"], "const:": ["v"], "let2": ["v", "w"],
    "const[]": ["v", "w"], "const[...]": ["v", "w"], "const{}": ["v", "w"],
    "const{=}": ["v", "w"], "function": ["f"], "function(p,q)": ["f"],
    "async function": ["f"], "function*": ["f"],
}


def export_of(text, shape, names, kind):
    entries = exported(0, "", *[(word, word) for word in names])
    made = Decl("export " + text, shape, "export", names, entries)
    made.desc = "export " + kind
    return made


# ------------------------------------------------------------ constructs

def declaration_atoms():
    t_type = Ty.T("Name", "T")
    mixed = [
        Member("x = 1;", [field_shape("x", init=S('IntLit 1 "1"'))], [], []),
        Member("m() { }", [], [method_shape("m", S("Block n=0"))], []),
        Member("y = 2;", [field_shape("y", init=S('IntLit 2 "2"'))], [], []),
        Member("constructor() { }", [], [],
               [method_shape("constructor", S("Block n=0"), role=2)]),
    ]
    name_d = S("Decorator", name_shape("d"))
    call_d = S("Decorator", S("Call opt=0 tagged=0 n=1 spread=-",
                              name_shape("d"), name_shape("a")))
    member_d = S("Decorator", S('Member "b" opt=0 targs=0', name_shape("a")))
    generic_base = Ty.T("Generic", "B", [t_type])
    generic_face = Ty.T("Generic", "C", [t_type])
    return [
        enum("enum E { }", []),
        enum("enum E { A }", [("A", MISSING)]),
        enum("enum E { A, B, C }",
             [("A", MISSING), ("B", MISSING), ("C", MISSING)]),
        enum("const enum E { A }", [("A", MISSING)]),
        Decl("declare class A {}", S('Handle "A"'), "declare class", ["A"]),
        Decl("interface I { }", alias("I", [], EMPTY_OBJECT), "interface",
             ["I"]),
        Decl("interface I<T> { a: T }",
             alias("I", [type_param("T")],
                   Ty.object_shape(["a"], [False], [False], [False],
                                   [t_type])), "interface", ["I"]),
        class_with([]),
        class_with([], "abstract class A", abstract=1),
        class_with([], "partial class A", partial=1),
        class_with([], "class A<T>", type_params=[type_param("T")]),
        class_with([], "class A<T> extends B<T> implements C<T>",
                   type_params=[type_param("T")], base=generic_base,
                   interfaces=[generic_face]),
        class_with(mixed),
        class_with([], "@d class A", decorators=[name_d]),
        class_with([], "@d(a) class A", decorators=[call_d]),
        class_with([], "@a.b class A", decorators=[member_d]),
        class_with([], "@d @a.b class A", decorators=[name_d, member_d]),
    ]


def constructs():
    made = []

    def add(label, slots, build):
        made.append(E.Construct(label, slots, build))

    add("enum{A=E}", ["expr"], enum_one)
    add("enum{A=E,B=F}", ["expr", "expr"], enum_two)
    add("enum{A,B=E,}", ["expr"], enum_trailing)
    for construct in Ty.constructs():
        if construct.name in OBJECT_KINDS:
            add("interface " + construct.name, ["type"] * construct.slots,
                interface_of(construct))
    add("interface extends", ["typeref"], interface_extends)
    add("interface extends,", ["typeref", "typeref"], interface_extends)
    add("interface<T extends=>", ["type", "type"], interface_generic)
    add("type<U>", ["type"], alias_generic)
    add("type<U extends=>", ["type", "type", "type"], alias_limited)
    add("function<T>", ["item"], function_generic)
    add("function typed",
        ["type", "expr", "type", "type", "type", "item"], function_typed)
    add("declare function", ["type", "type"], function_declared)
    add("function(this)", ["type"], function_this)
    add("function({},[])", ["type", "type", "item"], function_patterns)
    for member in members():
        add("class{%s}" % member.name, member.slots, class_of(member))
    add("class extends T", ["typeref"], class_extends_type)
    add("class extends E", ["expr"], class_extends_value)
    add("class extends E<T>", ["expr"], class_extends_typed_value)
    add("class implements", ["typeref"], class_implements)
    add("class implements,", ["typeref", "typeref"], class_implements)
    add("class<T extends=>", ["type", "type"], class_generic)
    add("@D class", ["expr"], class_decorated)
    add("abstract m(): T", ["type"], class_abstract_method)
    return made


TYPE_REFERENCES = ("A", "a.B", "A<>", "A<,>", "a.B<>")


def accepts(slot, part):
    if slot == "expr":
        return isinstance(part, E.Node)
    if slot == "item":
        return isinstance(part, St.Stmt)
    if not isinstance(part, Ty.Ty):
        return False
    return slot == "type" or part.kind in TYPE_REFERENCES


def plain_part(slot, word):
    if slot == "expr":
        return E.name(word)
    if slot == "item":
        return St.expression(E.name(word))
    return Ty.named(word.upper())


def fill(construct, deep, words):
    kids = []
    for at, slot in enumerate(construct.slots):
        given = deep.get(at)
        if given is None:
            kids.append(plain_part(slot, words[at]))
            continue
        if not accepts(slot, given):
            return None
        kids.append(given)
    made = construct.build(*kids)
    if made is None:
        return None
    made.kind = construct.name
    made.kids = tuple(kids)
    for kid in kids:
        if kid.fails:
            made.fails = kid.fails
    places = ",".join("%d=%s" % (at, deep[at].desc) for at in sorted(deep))
    made.desc = construct.name + ("[" + places + "]" if places else "")
    return made


# ----------------------------------------------------------- enumeration

def deepen(made, inner, slots, level):
    for construct in made:
        for at, slot in enumerate(construct.slots):
            if slot not in slots:
                continue
            for item in inner:
                decl = fill(construct, {at: item.node}, "bcdefg")
                if decl is None:
                    continue
                pair = (construct.name, at, item.node.kind)
                yield E.Item(level, (pair,) + item.pairs, decl)


def member_pairs():
    listed = []
    for member in members():
        slots = [plain_part(slot, word)
                 for slot, word in zip(member.slots, "bcdefg")]
        listed.append((member.name, member.build(*slots)))
    for first_name, first in listed:
        for second_name, second in listed:
            if first.fields and first.ctors and second.ctors:
                continue
            decl = class_with([first, second])
            decl.desc = "class{%s %s}" % (first_name, second_name)
            pairs = (("class", 0, first_name), ("class", 1, second_name))
            yield E.Item("P2", pairs, decl)


def exports(plain):
    for item in plain:
        node = item.node
        hidden = node.text.startswith(("@", "partial "))
        if node.declares and not hidden:
            yield E.Item("X", (("export", 0, node.kind),),
                         export_of(node.text, node.shape, node.declares,
                                   node.desc))
    for construct in St.constructs():
        names = VARIABLE_NAMES.get(construct.name)
        if names is None:
            continue
        stmt = St.fill(construct, {}, "abcd")
        yield E.Item("X", (("export", 0, construct.name),),
                     export_of(stmt.text, stmt.shape, names, construct.name))


def type_parts():
    parts = [E.Item("T", (), part) for part in Ty.atoms()]
    made = Ty.constructs()
    plain = [E.Item("T", (), Ty.fill(construct, {}, "ABCD"))
             for construct in made]
    return parts + plain, made, plain, parts


def enumerate_levels(levels):
    made = constructs()
    alone = [E.Item("A", (), decl)
             for decl in module_atoms() + declaration_atoms()]
    plain = []
    for construct in made:
        decl = fill(construct, {}, "bcdefg")
        if decl is not None:
            plain.append(E.Item("R1", (), decl))
    types, type_constructs, plain_types, type_atoms = type_parts()
    exprs = [E.Item("E", (), node) for node in St.expression_parts()]
    stmts = [E.Item("S", (), St.fill(construct, {}, "wxyz"))
             for construct in St.constructs()]
    stmts += [E.Item("S", (), stmt) for stmt in St.atoms()]
    kinds = ("type", "typeref")
    if "A" in levels:
        yield from alone
    if "R1" in levels:
        yield from plain
    if "X" in levels:
        yield from exports(alone + plain)
    if "T1" in levels:
        yield from deepen(made, types, kinds, "T1")
    if "E1" in levels:
        yield from deepen(made, exprs, ("expr",), "E1")
    if "S1" in levels:
        yield from deepen(made, stmts, ("item",), "S1")
    if "P2" in levels:
        yield from member_pairs()
    if "M1" in levels:
        wrong = [E.Item("M", (), node) for node in E.mistakes()]
        yield from deepen(made, wrong, ("expr",), "M1")
    if "T2" in levels:
        deeper = list(Ty.one_deep(type_constructs, type_atoms, "ABCD", "T"))
        deeper += list(Ty.one_deep(type_constructs, plain_types, "WXYZ", "T"))
        yield from deepen(made, deeper, kinds, "T2")
    if "E2" in levels:
        plain_exprs = [item for item in exprs if item.node.kids]
        pairs = E.one_deep(E.constructs(), plain_exprs, "xyz", "E")
        yield from deepen(made, list(pairs), ("expr",), "E2")


if __name__ == "__main__":
    os.makedirs(sys.argv[2], exist_ok=True)
    E.main(enumerate_levels,
           ["A", "R1", "X", "T1", "E1", "S1", "P2", "M1", "T2", "E2"], "none")
