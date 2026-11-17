"""Enumerates tz expressions together with the trees they must give and
checks the self/ parser against them.

    python exprgen.py <tzparse.exe> <work dir> [levels] [bare]

'bare' writes every expression as a statement without parentheses around
it ('a + b;' instead of '(a + b);'); expressions that cannot start a
statement are left out.  An expression with 'yield' is put into a generator.

levels: any of A R1 L1 L2 S2 L3 M1 M2 separated by commas, or 'all'
(default: all but L3).  M1 and M2 are expressions the parser must refuse:
there the check is that it reports the one expected error and no other.

The script exits with 1 when an expression fails, and so do the scripts
built on it (stmtgen, typegen, declgen).

Every expression is built from constructs, so the builder knows its tree.
The text is printed with the fewest parentheses the grammar allows; the
rules for that come from the ECMAScript grammar and docs/, not from the
parser.  An expression fails when the parser reports an error for it or
builds another tree.

Where tz knowingly differs from ECMAScript the generator follows tz:
  - after 'new' comes a name or 'this', its members and indexes, or
    parentheses; 'new 1()', 'new class { }()', 'new new A()()' are not
    generated;
  - the base of a class expression is a name or what starts with a name
    and goes on with '.', '[', '(' or type arguments and '(', or any other
    primary expression, and type arguments may follow a base that is not
    a plain name; 'class extends a?.b { }' and 'class extends tag`t` { }'
    are not generated;
  - a '/' right after '}' starts a regular expression, so that a statement
    after a block may begin with one; '{} / 2' and 'class { } / 2' are not
    generated;
  - 'a < b > (c)' is the call 'a<b>(c)' (docs/generics.md, section 3):
    the builder of '>' gives the tree of that call; other texts that start
    like it are not generated.
"""

import bisect
import collections
import os
import re
import struct
import subprocess
import sys
import time

(COMMA, ASSIGN, COND, NULLISH, LOGOR, LOGAND, BITOR, BITXOR, BITAND, EQUAL,
 RELATION, SHIFT, ADD, MUL, POWER, UNARY, UPDATE, NEW_BARE, CALL,
 PRIMARY) = range(1, 21)

CHUNK = 100000


class Node:
    __slots__ = ("text", "prec", "shape", "kind", "chain", "member",
                 "target", "named", "desc", "kids", "fails")

    def __init__(self, text, prec, shape, kind, chain=False, member=False,
                 target=False, named=None):
        self.text = text
        self.prec = prec
        self.shape = shape
        self.kind = kind
        self.chain = chain
        self.member = member
        self.target = target
        self.named = named
        self.desc = kind
        self.kids = ()
        self.fails = None


def S(label, *kids):
    return (label, kids)


def wrap(child, least):
    if child.prec < least:
        return "(" + child.text + ")", True
    return child.text, False


def bits(value):
    return struct.pack(">d", value).hex()


# ---------------------------------------------------------------- leaves

def name(word):
    shape = S('Name "%s" targs=0' % word)
    return Node(word, PRIMARY, shape, "name", member=True, target=True,
                named=(word, ""))


EMPTY_BLOCK = S("Block n=0")
MISSING = S("-")


def function_shape(own, arrow, concise, is_async, params, body,
                   result=MISSING, type_params=()):
    head = ('Function "%s" arrow=%d concise=%d own="" place=1 body=0 gen=0 '
            'async=%d role=plain tp=%d params=%d'
            % (own, arrow, concise, is_async, len(type_params), len(params)))
    return S(head, *type_params, *params, result, body)


def atoms():
    def literal(text, label, kind):
        return Node(text, PRIMARY, S(label), kind)

    method = function_shape("m", 0, 0, 0, [], EMPTY_BLOCK)
    plain_function = function_shape("", 0, 0, 0, [], EMPTY_BLOCK)
    empty_arrow = function_shape("", 1, 0, 0, [], EMPTY_BLOCK)
    class_line = ('Class "" abstract=0 partial=0 expr=1 generic=0 tp=0 '
                  'base=0 baseExpr=0 baseArgs=0 ifaces=0 dec=0 fields=0 '
                  'methods=0 ctors=0')
    return [
        Node("this", PRIMARY, S('Name "this" targs=0'), "this", member=True),
        literal("1", 'IntLit 1 "1"', "int"),
        literal("0x1f", 'IntLit 31 "0x1f"', "hex"),
        literal("1.5", 'FloatLit %s "1.5" beyond=0' % bits(1.5), "float"),
        literal("5.", 'FloatLit %s "5." beyond=0' % bits(5.0), "float."),
        literal("1e3", 'FloatLit %s "1e3" beyond=0' % bits(1000.0), "exp"),
        literal("1e400", 'FloatLit %s "1e400" beyond=1' % bits(0.0), "huge"),
        literal('"s"', 'StringLit "s"', 'str"'),
        literal("'s'", 'StringLit "s"', "str'"),
        literal("true", "BoolLit 1", "true"),
        literal("false", "BoolLit 0", "false"),
        literal("undefined", "Undefined", "undefined"),
        literal("/r/g", 'RegexLit "r" "g"', "regex"),
        literal("`t`", 'Template n=1 "t"', "tpl0"),
        literal("[]", "ArrayLit n=0", "[]"),
        literal("{}", "ObjectLit n=0", "{}"),
        Node("{ k }", PRIMARY,
             S("ObjectLit n=1",
               S('Property "k" Value', S('Name "k" targs=0'))), "{k}"),
        Node("{ m() { } }", PRIMARY,
             S("ObjectLit n=1",
               S('Property "m" Method', S("FunctionExpr", method))), "{m()}"),
        Node("function () { }", PRIMARY, S("FunctionExpr", plain_function),
             "function"),
        Node("class { }", PRIMARY, S("ClassExpr", S(class_line)), "class"),
        Node("class A { }", PRIMARY,
             S("ClassExpr", S(class_line.replace('Class ""', 'Class "A"'))),
             "class A"),
        Node("class { m() { } }", PRIMARY,
             S("ClassExpr",
               S(class_line.replace("methods=0", "methods=1"),
                 S('Function "m" arrow=0 concise=0 own="" place=0 body=0 '
                   'gen=0 async=0 role=member 0 0 0 0 tp=0 params=0',
                   MISSING, EMPTY_BLOCK))), "class{m()}"),
        Node("class extends B { }", PRIMARY,
             S("ClassExpr",
               S(class_line.replace("base=0 baseExpr=0", "base=1 baseExpr=1"),
                 B_TYPE, S('Name "B" targs=0'))), "class extends B"),
        Node("yield", ASSIGN, S("Yield delegate=0", MISSING), "yield"),
        Node("() => { }", ASSIGN, S("FunctionExpr", empty_arrow), "()=>{}"),
    ]


# ------------------------------------------------------------- operators

BINARY = [
    ("+", 0, ADD), ("-", 1, ADD), ("*", 2, MUL), ("/", 3, MUL),
    ("%", 4, MUL), ("**", 5, POWER), ("&", 6, BITAND), ("|", 7, BITOR),
    ("^", 8, BITXOR), ("<<", 9, SHIFT), (">>", 10, SHIFT),
    (">>>", 11, SHIFT), ("<", 12, RELATION), ("<=", 13, RELATION),
    (">", 14, RELATION), (">=", 15, RELATION), ("==", 16, EQUAL),
    ("!=", 17, EQUAL), ("&&", 18, LOGAND), ("||", 19, LOGOR),
    ("??", 20, NULLISH), ("in", 21, RELATION), ("instanceof", 22, RELATION),
]

UNARY_SIGNS = [
    ("-", 0), ("+", 1), ("!", 2), ("~", 3), ("typeof", 4), ("void", 5),
    ("delete", 6),
]

ASSIGNS = [
    ("=", None), ("+=", 0), ("-=", 1), ("*=", 2), ("/=", 3), ("%=", 4),
    ("**=", 5), ("&=", 6), ("|=", 7), ("^=", 8), ("<<=", 9), (">>=", 10),
    (">>>=", 11), ("&&=", 18), ("||=", 19), ("??=", 20),
]


def generic_call_instead(left, right, text):
    """The tree of 'a < b > (c)': the call 'a<b>(c)'.  None when the left
    side is not 'name < name', where the rule of tz is not written down."""
    names = [kid for kid in left.kids if kid.kind == "name"]
    if len(names) != 2:
        return None
    inner = right.kids[0] if right.kind == "paren" else right
    args = inner.shape[1] if inner.prec == COMMA else (inner.shape,)
    callee = S('Name "%s" targs=1' % names[0].text,
               S('Type Name "%s" n=0' % names[1].text))
    label = "Call opt=0 tagged=0 n=%d spread=-" % len(args)
    return Node(text, CALL, S(label, callee, *args), ">")


def binary(sign, number, level):
    def build(left, right):
        if sign == ">" and left.kind == "<":
            right_text = wrap(right, level + 1)[0]
            text = "%s > %s" % (left.text, right_text)
            if right.kind == "paren" or right.prec <= level:
                return generic_call_instead(left, right, text)
            if right_text.startswith("("):
                return None
        before = wrap(left, level)[0]
        if before.endswith(AFTER_AS) and sign in ("|", "&", "<"):
            return None
        if before.rstrip("!").endswith("}") and sign == "/":
            return None
        if sign == "**":
            left_text = wrap(left, UPDATE)[0]
            right_text = wrap(right, POWER)[0]
        elif sign == "??":
            left_text = left.text if left.kind == "??" else wrap(left, BITOR)[0]
            right_text = wrap(right, BITOR)[0]
        else:
            left_text = wrap(left, level)[0]
            right_text = wrap(right, level + 1)[0]
        text = "%s %s %s" % (left_text, sign, right_text)
        shape = S("Binary %d" % number, left.shape, right.shape)
        return Node(text, level, shape, sign)
    return build


def unary(sign, number):
    def build(operand):
        text = wrap(operand, UNARY)[0]
        glued = sign in "+-" and text.startswith(sign)
        gap = " " if sign.isalpha() or glued else ""
        shape = S("Unary %d" % number, operand.shape)
        return Node(sign + gap + text, UNARY, shape, "pre " + sign)
    return build


def await_(operand):
    text = "await " + wrap(operand, UNARY)[0]
    return Node(text, UNARY, S("Await", operand.shape), "await")


def update(increment, prefix):
    sign = "++" if increment else "--"

    def build(target):
        text = sign + target.text if prefix else target.text + sign
        label = "Update inc=%d prefix=%d" % (increment, prefix)
        kind = sign + "x" if prefix else "x" + sign
        return Node(text, UPDATE, S(label, target.shape), kind)
    return build


def assign(sign, number):
    def build(target, value):
        text = "%s %s %s" % (target.text, sign, wrap(value, ASSIGN)[0])
        label = "Assign -" if number is None else "Assign %d" % number
        return Node(text, ASSIGN, S(label, target.shape, value.shape), sign)
    return build


def conditional(cond, then, other):
    text = "%s ? %s : %s" % (wrap(cond, NULLISH)[0], wrap(then, ASSIGN)[0],
                             wrap(other, ASSIGN)[0])
    shape = S("Conditional", cond.shape, then.shape, other.shape)
    return Node(text, COND, shape, "?:")


def sequence(*parts):
    text = ", ".join(wrap(part, ASSIGN)[0] for part in parts)
    shape = S("Sequence n=%d" % len(parts), *[part.shape for part in parts])
    return Node(text, COMMA, shape, "seq%d" % len(parts))


B_TYPE = S('Type Name "B" n=0')


C_TYPE = S('Type Name "C" n=0')
AFTER_AS = (" as B", " as B | C", " as B & C")


def cast(operand):
    text = wrap(operand, RELATION)[0] + " as B"
    return Node(text, RELATION, S("Cast", operand.shape, B_TYPE), "as")


def cast_to_several(sign, kind):
    """'|' and '&' after 'as B' go on with the type, they are not operators
    of the expression."""
    def build(operand):
        text = "%s as B %s C" % (wrap(operand, RELATION)[0], sign)
        target = S('Type %s "" n=2' % kind, B_TYPE, C_TYPE)
        return Node(text, RELATION, S("Cast", operand.shape, target),
                    "as " + sign)
    return build


def yield_(delegate):
    def build(value):
        star = "* " if delegate else " "
        text = "yield" + star + wrap(value, ASSIGN)[0]
        label = "Yield delegate=%d" % delegate
        return Node(text, ASSIGN, S(label, value.shape), "yield")
    return build


NAME_AT_START = re.compile("[A-Za-z_][A-Za-z_0-9]*")


def class_extends(base, typed=0):
    text = wrap(base, CALL)[0]
    name_at_start = NAME_AT_START.match(text)
    if name_at_start and name_at_start.group() != "this":
        after = text[name_at_start.end():name_at_start.end() + 1]
        if after not in (".", "[", "(", "<"):
            return None
    line = ('Class "" abstract=0 partial=0 expr=1 generic=0 tp=0 base=0 '
            'baseExpr=1 baseArgs=%d ifaces=0 dec=0 fields=0 methods=0 ctors=0'
            % typed)
    kids = [base.shape] + [INT_TYPE] * typed
    text = "class extends " + wrap(base, CALL)[0] + "<int>" * typed + " { }"
    return Node(text, PRIMARY, S("ClassExpr", S(line, *kids)), "class")


def class_extends_typed(base):
    return class_extends(base, typed=1)


def paren(inner):
    return Node("(" + inner.text + ")", PRIMARY, inner.shape, "paren",
                member=True)


# ---------------------------------------------------- members and calls

def chain_base(base, force=False):
    text, wrapped = wrap(base, CALL)
    if force and not wrapped:
        text, wrapped = "(" + text + ")", True
    continuing = base.chain and not wrapped
    inner = base.shape[1][0] if continuing else base.shape
    return text, wrapped, continuing, inner


def chain_end(shape, continuing, optional):
    if continuing or optional:
        return S("OptionalChain", shape), True
    return shape, False


def non_null(base):
    text, wrapped, continuing, inner = chain_base(base)
    shape, chain = chain_end(S("Unary 7", inner), continuing, False)
    return Node(text + "!", CALL, shape, "x!", chain=chain)


def member(prop, optional=False):
    def build(base):
        force = base.kind == "int" and not optional
        text, wrapped, continuing, inner = chain_base(base, force)
        label = 'Member "%s" opt=%d targs=0' % (prop, optional)
        shape, chain = chain_end(S(label, inner), continuing, optional)
        named = None
        if not chain and not wrapped and base.kind == "name":
            named = (base.text, prop)
        dot = "?." if optional else "."
        return Node(text + dot + prop, CALL, shape,
                    "?.m" if optional else ".m", chain=chain,
                    member=not chain and (wrapped or base.member),
                    target=not chain, named=named)
    return build


def index(optional=False):
    def build(base, key):
        text, wrapped, continuing, inner = chain_base(base)
        label = "Index opt=%d" % optional
        shape, chain = chain_end(S(label, inner, key.shape), continuing,
                                 optional)
        opening = "?.[" if optional else "["
        return Node(text + opening + wrap(key, COMMA)[0] + "]", CALL, shape,
                    "?.[]" if optional else "[]", chain=chain,
                    member=not chain and (wrapped or base.member),
                    target=not chain)
    return build


def arguments(args, spreads):
    parts = []
    for arg, spread in zip(args, spreads):
        parts.append(("..." if spread else "") + wrap(arg, ASSIGN)[0])
    flags = "-"
    if any(spreads):
        flags = "".join("1" if spread else "0" for spread in spreads)
    return ", ".join(parts), flags


def call(spreads, optional=False, kind=None):
    def build(callee, *args):
        text, wrapped, continuing, inner = chain_base(callee)
        listed, flags = arguments(args, spreads)
        label = ("Call opt=%d tagged=0 n=%d spread=%s"
                 % (optional, len(args), flags))
        shape, chain = chain_end(S(label, inner, *[a.shape for a in args]),
                                 continuing, optional)
        opening = "?.(" if optional else "("
        return Node(text + opening + listed + ")", CALL, shape, kind,
                    chain=chain, member=False)
    return build


INT_TYPE = S('Type Name "int" n=0')


def generic_call(on_member):
    def build(arg):
        if on_member:
            callee = S('Member "f" opt=0 targs=1', S('Name "a" targs=0'),
                       INT_TYPE)
            head = "a.f<int>("
        else:
            callee = S('Name "f" targs=1', INT_TYPE)
            head = "f<int>("
        shape = S("Call opt=0 tagged=0 n=1 spread=-", callee, arg.shape)
        kind = "a.f<T>()" if on_member else "f<T>()"
        return Node(head + wrap(arg, ASSIGN)[0] + ")", CALL, shape, kind,
                    member=False)
    return build


def new(bare, kind, spreads=()):
    def build(callee, *args):
        spreading = list(spreads) or [False] * len(args)
        listed, flags = arguments(args, spreading)
        wrapped = not callee.member
        text = "(" + callee.text + ")" if wrapped else callee.text
        named = None if wrapped else callee.named
        kids = [arg.shape for arg in args]
        if named is None:
            label = 'New "" "" targs=0 n=%d callee=1 spread=%s' % (
                len(args), flags)
            kids.insert(0, callee.shape)
        else:
            label = 'New "%s" "%s" targs=0 n=%d callee=0 spread=%s' % (
                named[0], named[1], len(args), flags)
        if bare:
            return Node("new " + text, NEW_BARE, S(label, *kids), "new",
                        member=False)
        return Node("new %s(%s)" % (text, listed), CALL, S(label, *kids),
                    kind)
    return build


def generic_new(on_member):
    def build(arg):
        names = ("a", "B") if on_member else ("A", "")
        label = 'New "%s" "%s" targs=1 n=1 callee=0 spread=-' % names
        head = "new a.B<int>(" if on_member else "new A<int>("
        kind = "new a.B<T>()" if on_member else "new A<T>()"
        return Node(head + wrap(arg, ASSIGN)[0] + ")", CALL,
                    S(label, INT_TYPE, arg.shape), kind)
    return build


def string_element(text):
    return S("Element 0 0", S('StringLit "%s"' % text))


def tagged(holes):
    def build(tag, *exprs):
        text, wrapped, continuing, inner = chain_base(tag, force=tag.chain)
        quasis = ["t"] if holes == 0 else ["p", "q"]
        strings = S("ArrayLit n=%d" % len(quasis),
                    *[string_element(q) for q in quasis])
        label = "Call opt=0 tagged=1 n=%d spread=-" % (1 + len(exprs))
        shape = S(label, inner, strings, *[e.shape for e in exprs])
        body = "t" if holes == 0 else "p${" + wrap(exprs[0], COMMA)[0] + "}q"
        return Node(text + "`" + body + "`", CALL, shape, "tag%d" % holes)
    return build


def template(holes):
    def build(*exprs):
        if holes == 1:
            body = "p${" + wrap(exprs[0], COMMA)[0] + "}q"
            label = 'Template n=2 "p" "q"'
        else:
            body = ("${" + wrap(exprs[0], COMMA)[0] + "}${"
                    + wrap(exprs[1], COMMA)[0] + "}")
            label = 'Template n=3 "" "" ""'
        shape = S(label, *[e.shape for e in exprs])
        return Node("`" + body + "`", PRIMARY, shape, "tpl%d" % holes)
    return build


# ------------------------------------------------- arrays and objects

def array(pattern, kind):
    """pattern: 'x' element, '.' spread element, '_' hole, ',' trailing."""
    def build(*exprs):
        rest = list(exprs)
        texts, kids = [], []
        for mark in pattern:
            if mark == "_":
                texts.append("")
                kids.append(S("Element 0 1", S("Undefined")))
            elif mark == ",":
                texts.append(None)
            else:
                expr = rest.pop(0)
                spread = mark == "."
                texts.append(("..." if spread else "")
                             + wrap(expr, ASSIGN)[0])
                kids.append(S("Element %d 0" % spread, expr.shape))
        trailing = texts and texts[-1] is None
        if trailing:
            texts.pop()
        text = "[" + ", ".join(texts) + ("," if trailing else "") + "]"
        return Node(text, PRIMARY, S("ArrayLit n=%d" % len(kids), *kids), kind)
    return build


def object_(form, kind):
    def build(*exprs):
        values = [wrap(expr, ASSIGN)[0] for expr in exprs]
        shapes = [expr.shape for expr in exprs]
        if form == "value":
            text = "{ k: %s }" % values[0]
            props = [S('Property "k" Value', shapes[0])]
        elif form == "two":
            text = "{ k: %s, m: %s }" % (values[0], values[1])
            props = [S('Property "k" Value', shapes[0]),
                     S('Property "m" Value', shapes[1])]
        elif form == "computed":
            text = "{ [%s]: %s }" % (values[0], values[1])
            props = [S('Property "" Computed', shapes[0], shapes[1])]
        elif form == "spread":
            text = "{ ...%s }" % values[0]
            props = [S('Property "" Spread omit=[] keys=[]', shapes[0])]
        elif form == "string":
            text = '{ "k": %s }' % values[0]
            props = [S('Property "k" Value', shapes[0])]
        else:
            text = "{ k: %s, }" % values[0]
            props = [S('Property "k" Value', shapes[0])]
        return Node(text, PRIMARY, S("ObjectLit n=%d" % len(props), *props),
                    kind)
    return build


# ---------------------------------------------------------------- arrows

def parameter(word, form=0, default=None, type_=MISSING):
    label = 'Param "%s" form=%d prop=- dec=0' % (word, form)
    if default is None:
        return S(label, type_)
    return S(label, type_, default)


def arrow_body(body):
    text = wrap(body, ASSIGN)[0]
    if text.startswith("{"):
        text = "(" + text + ")"
    return text


def awaits(node):
    return "await " in node.text


def yields(node):
    return "yield" in node.text


def arrow(head, params, is_async, kind, result=MISSING, type_params=()):
    def build(body):
        if (awaits(body) and not is_async) or yields(body):
            return None
        block = S("Block n=1", S("Return", body.shape))
        shape = function_shape("", 1, 1, is_async,
                               [parameter(*param) for param in params], block,
                               result, type_params)
        return Node(head + " => " + arrow_body(body), ASSIGN,
                    S("FunctionExpr", shape), kind)
    return build


def arrow_default(typed):
    def build(default, body):
        if awaits(default) or awaits(body) or yields(default) or yields(body):
            return None
        block = S("Block n=1", S("Return", body.shape))
        type_ = INT_TYPE if typed else MISSING
        shape = function_shape(
            "", 1, 1, 0, [parameter("p", 2, default.shape, type_)], block)
        head = "(p: int = %s)" if typed else "(p = %s)"
        text = "%s => %s" % (head % wrap(default, ASSIGN)[0],
                             arrow_body(body))
        kind = "(p:T=x)=>y" if typed else "(p=x)=>y"
        return Node(text, ASSIGN, S("FunctionExpr", shape), kind)
    return build


def arrow_block(value):
    if awaits(value) or yields(value):
        return None
    block = S("Block n=1", S("Return", value.shape))
    shape = function_shape("", 1, 0, 0, [], block)
    text = "() => { return %s; }" % wrap(value, COMMA)[0]
    return Node(text, ASSIGN, S("FunctionExpr", shape), "()=>{return x}")


# ------------------------------------------------------------ constructs

Construct = collections.namedtuple("Construct", "name slots build")


def constructs():
    made = []

    def add(label, slots, build):
        made.append(Construct(label, slots, build))

    for sign, number, level in BINARY:
        add(sign, ["expr", "expr"], binary(sign, number, level))
    for sign, number in UNARY_SIGNS:
        slot = "delete" if sign == "delete" else "expr"
        add("pre " + sign, [slot], unary(sign, number))
    add("await", ["expr"], await_)
    add("++x", ["target"], update(True, True))
    add("--x", ["target"], update(False, True))
    add("x++", ["target"], update(True, False))
    add("x--", ["target"], update(False, False))
    for sign, number in ASSIGNS:
        add(sign, ["target", "expr"], assign(sign, number))
    add("?:", ["expr", "expr", "expr"], conditional)
    add(".m", ["expr"], member("m"))
    add("?.m", ["expr"], member("m", True))
    add(".delete", ["expr"], member("delete"))
    add("?.typeof", ["expr"], member("typeof", True))
    add("[]", ["expr", "expr"], index())
    add("?.[]", ["expr", "expr"], index(True))
    add("f()", ["expr"], call([], kind="f()"))
    add("f(x)", ["expr", "expr"], call([False], kind="f(x)"))
    add("f(x,y)", ["expr", "expr", "expr"],
        call([False, False], kind="f(x,y)"))
    add("f(...x)", ["expr", "expr"], call([True], kind="f(...x)"))
    add("f(x,...y)", ["expr", "expr", "expr"],
        call([False, True], kind="f(x,...y)"))
    add("f?.(x)", ["expr", "expr"], call([False], True, kind="f?.(x)"))
    add("f<T>()", ["expr"], generic_call(False))
    add("a.f<T>()", ["expr"], generic_call(True))
    add("new A()", ["expr"], new(False, "new A()"))
    add("new A(x)", ["expr", "expr"], new(False, "new A(x)"))
    add("new A(...x)", ["expr", "expr"], new(False, "new A(...x)", [True]))
    add("new A", ["expr"], new(True, "new"))
    add("new A<T>()", ["expr"], generic_new(False))
    add("new a.B<T>()", ["expr"], generic_new(True))
    add("tag0", ["expr"], tagged(0))
    add("tag1", ["expr", "expr"], tagged(1))
    add("tpl1", ["expr"], template(1))
    add("tpl2", ["expr", "expr"], template(2))
    add("[x]", ["expr"], array("x", "[x]"))
    add("[x,y]", ["expr", "expr"], array("xx", "[x,y]"))
    add("[...x]", ["expr"], array(".", "[...x]"))
    add("[,x]", ["expr"], array("_x", "[,x]"))
    add("[x,,y]", ["expr", "expr"], array("x_x", "[x,,y]"))
    add("[x,]", ["expr"], array("x,", "[x,]"))
    add("{k:x}", ["expr"], object_("value", "{k:x}"))
    add("{k:x,m:y}", ["expr", "expr"], object_("two", "{k:x,m:y}"))
    add("{[x]:y}", ["expr", "expr"], object_("computed", "{[x]:y}"))
    add("{...x}", ["expr"], object_("spread", "{...x}"))
    add('{"k":x}', ["expr"], object_("string", '{"k":x}'))
    add("{k:x,}", ["expr"], object_("trailing", "{k:x,}"))
    add("p=>x", ["expr"], arrow("p", [("p",)], 0, "p=>x"))
    add("(p)=>x", ["expr"], arrow("(p)", [("p",)], 0, "(p)=>x"))
    add("()=>x", ["expr"], arrow("()", [], 0, "()=>x"))
    add("(p,q)=>x", ["expr"],
        arrow("(p, q)", [("p",), ("q",)], 0, "(p,q)=>x"))
    add("async(p)=>x", ["expr"], arrow("async (p)", [("p",)], 1,
                                       "async(p)=>x"))
    add("async p=>x", ["expr"], arrow("async p", [("p",)], 1, "async p=>x"))
    add("(...p)=>x", ["expr"], arrow("(...p)", [("p", 3)], 0, "(...p)=>x"))
    add("(p):T=>x", ["expr"],
        arrow("(p): int", [("p",)], 0, "(p):T=>x", INT_TYPE))
    add("(p=x)=>y", ["expr", "expr"], arrow_default(False))
    add("(p:T=x)=>y", ["expr", "expr"], arrow_default(True))
    add("(p:T)=>x", ["expr"],
        arrow("(p: int)", [("p", 0, None, INT_TYPE)], 0, "(p:T)=>x"))
    add("(p?:T)=>x", ["expr"],
        arrow("(p?: int)", [("p", 1, None, INT_TYPE)], 0, "(p?:T)=>x"))
    type_param = S('TypeParam "T"', MISSING, MISSING)
    t_type = S('Type Name "T" n=0')
    add("<T>(p:T)=>x", ["expr"],
        arrow("<T>(p: T)", [("p", 0, None, t_type)], 0, "<T>(p:T)=>x",
              type_params=(type_param,)))
    add("async<T>(p)=>x", ["expr"],
        arrow("async <T>(p)", [("p",)], 1, "async<T>(p)=>x",
              type_params=(type_param,)))
    add("as", ["expr"], cast)
    add("as |", ["expr"], cast_to_several("|", "Union"))
    add("as &", ["expr"], cast_to_several("&", "Intersection"))
    add("x!", ["expr"], non_null)
    add("yield x", ["expr"], yield_(False))
    add("yield* x", ["expr"], yield_(True))
    add("class extends", ["expr"], class_extends)
    add("class extends<T>", ["expr"], class_extends_typed)
    add("()=>{return x}", ["expr"], arrow_block)
    add("seq2", ["expr", "expr"], sequence)
    add("seq3", ["expr", "expr", "expr"], sequence)
    add("paren", ["expr"], paren)
    return made


def accepts(slot, node):
    if slot == "target":
        return node.target
    if slot == "delete":
        return not node.shape[0].startswith("Name ")
    return True


def default_leaf(slot, word):
    if slot == "delete":
        return member("k")(name(word))
    return name(word)


def fill(construct, deep, words):
    """deep: {slot index: node}; the other slots get plain names."""
    kids = []
    for at, slot in enumerate(construct.slots):
        given = deep.get(at)
        if given is None:
            kids.append(default_leaf(slot, words[at]))
            continue
        if not accepts(slot, given):
            return None
        kids.append(given)
    node = construct.build(*kids)
    if node is None:
        return None
    node.kind = construct.name
    node.kids = tuple(kids)
    for kid in kids:
        if kid.fails:
            node.fails = kid.fails
    places = ",".join("%d=%s" % (at, deep[at].desc) for at in sorted(deep))
    node.desc = construct.name + ("[" + places + "]" if places else "")
    return node


# -------------------------------------------------------------- mistakes

def mistakes():
    """Expressions the parser must refuse, each with the code of its error:
    '??' next to '||' or '&&' without parentheses, a change of what '?.'
    reaches, '?.' on the way to the class of 'new', and an assignment to a
    pattern that is not a statement of its own."""
    made = []

    def add(text, prec, code, kind, chain=False):
        shape = S("OptionalChain", S("?")) if chain else S("?")
        node = Node(text, prec, shape, kind, chain=chain)
        node.fails = code
        made.append(node)

    for text in ("a ?? b || c", "a || b ?? c", "a ?? b && c", "a && b ?? c"):
        add(text, NULLISH, "TZ1233", "mix " + text)
    reached = ("a?.m", "a?.[b]", "a?.m.k")
    for target in reached:
        for sign, _ in ASSIGNS:
            add("%s %s c" % (target, sign), ASSIGN, "TZ1234",
                "%s %s" % (target, sign))
        for sign in ("++", "--"):
            add(sign + target, UPDATE, "TZ1234", sign + target)
            add(target + sign, UPDATE, "TZ1234", target + sign)
    for text in ("new a?.m()", "new a?.m", "new a?.m(b)", "new a?.[b]()",
                 "new a?.()"):
        add(text, CALL, "TZ1235", text, chain=True)
    for sign, _ in ASSIGNS:
        if sign != "=":
            add("[a, b] %s c" % sign, ASSIGN, "TZ1236", "[a, b] " + sign)
    for text in ("d = [a, b] = c", "d = { a } = c", "d += [a, b] = c"):
        add(text, ASSIGN, "TZ1236", text)
    return made


# ----------------------------------------------------------- enumeration

Item = collections.namedtuple("Item", "level pairs node")


def one_deep(made, inner_items, words, level):
    for construct in made:
        for at in range(len(construct.slots)):
            for inner in inner_items:
                node = fill(construct, {at: inner.node}, words)
                if node is None:
                    continue
                pair = (construct.name, at, inner.node.kind)
                yield Item(level, (pair,) + inner.pairs, node)


def two_deep(made, inner_items, words):
    for construct in made:
        count = len(construct.slots)
        for first in range(count):
            for second in range(first + 1, count):
                for left in inner_items:
                    for right in inner_items:
                        node = fill(construct,
                                    {first: left.node, second: right.node},
                                    words)
                        if node is None:
                            continue
                        pairs = ((construct.name, first, left.node.kind),
                                 (construct.name, second, right.node.kind))
                        yield Item("S2", pairs, node)


def enumerate_levels(levels):
    made = constructs()
    atom_items = [Item("A", (), atom) for atom in atoms()]
    plain = []
    for construct in made:
        node = fill(construct, {}, "abc")
        if node is not None:
            plain.append(Item("R1", (), node))
    if "A" in levels:
        yield from atom_items
    if "R1" in levels:
        yield from plain
    if "L1" in levels:
        yield from one_deep(made, atom_items, "abc", "L1")
    pairs = None
    if "L2" in levels or "L3" in levels:
        pairs = list(one_deep(made, plain, "xyz", "L2"))
    if "L2" in levels:
        yield from pairs
    if "S2" in levels:
        yield from two_deep(made, plain, "xyz")
    if "L3" in levels:
        yield from one_deep(made, pairs, "uvw", "L3")
    wrong = [Item("M1", (), node) for node in mistakes()]
    if "M1" in levels:
        yield from wrong
    if "M2" in levels:
        yield from one_deep(made, wrong, "xyz", "M2")


# --------------------------------------------------------------- running

LOCATION = re.compile(r"^( *)(?!Element )([A-Za-z]+) (-?\d+)")
SPAN = re.compile(r" src=(\d+)-(\d+)")
DROPPED = re.compile(r" (?:src|end|written|pattern|locs)=\S+")
ERROR_LINE = re.compile(r":(\d+):\d+: error (TZ\d+): (.*)$")


def flatten(shape, depth, out):
    out.append("  " * depth + shape[0])
    for kid in shape[1]:
        flatten(kid, depth + 1, out)


def in_generator(node):
    return "yield" in node.text


GENERATOR_LINE = ('Function "g" arrow=0 concise=0 own="" place=0 body=0 '
                  'gen=1 async=%d role=plain tp=0 params=0')


def expected_text(node):
    """All the lines the dump must have for the statement of this node."""
    ready = getattr(node, "block", None)
    if ready is not None:
        return ready
    if in_generator(node):
        out = [GENERATOR_LINE % awaits(node), "  -", "  Block n=1",
               "    ExprStmt"]
        flatten(node.shape, 3, out)
    else:
        out = ["ExprStmt"]
        flatten(node.shape, 1, out)
    return "\n".join(out)


BARE = False
NOT_AT_START = ("{", "function", "class", "async function", "let")


def statement(node):
    """One line of source with the expression as a statement of its own."""
    ready = getattr(node, "line", None)
    if ready is not None:
        return ready
    text = node.text + ";" if BARE else "(" + node.text + ");"
    if not in_generator(node):
        return text + "\n"
    head = "async function* g() { " if awaits(node) else "function* g() { "
    return head + text + " }\n"


TEMPORARY = re.compile(r'"%\d+"')


def normalize(line):
    line = DROPPED.sub("", LOCATION.sub(r"\1\2", line))
    if "%" in line:
        line = TEMPORARY.sub('"%"', line)
    return line


def outside(line, start, end):
    """Does the line name a place that is not inside its own statement?"""
    found = LOCATION.match(line)
    if found and not start <= int(found.group(3)) < end:
        return True
    span = SPAN.search(line)
    if span is None:
        return False
    first, last = int(span.group(1)), int(span.group(2))
    return not start <= first < last <= end


QUOTED = r'"(?:[^"\\]|\\.)*"'
IMPORT = re.compile(r"^Import (-?\d+) (%s) -?\d+ (%s) -?\d+ (\d) (\d) (\d+)$"
                    % (QUOTED, QUOTED))
EXPORT = re.compile(r"^Export (-?\d+) (\d) (%s) -?\d+ (\d+)$" % QUOTED)
MODULE_NAME = re.compile(r"^  Name (%s) -?\d+ (%s) -?\d+$" % (QUOTED, QUOTED))


def module_entry(line):
    """An entry of the tables of imports and exports after the statements:
    where it is in the source and how it is written without positions."""
    found = IMPORT.match(line)
    if found:
        return int(found.group(1)), "Import %s %s %s %s %s" % found.groups()[1:]
    found = EXPORT.match(line)
    if found:
        return int(found.group(1)), "Export %s %s %s" % found.groups()[1:]
    return None


class Failure:
    __slots__ = ("item", "kind", "detail", "actual")

    def __init__(self, item, kind, detail, actual):
        self.item = item
        self.kind = kind
        self.detail = detail
        self.actual = actual


def run_chunk(exe, work, number, items):
    path = os.path.join(work, "chunk%03d.tz" % number)
    offsets = []
    at = 0
    with open(path, "w", newline="\n") as out:
        for item in items:
            line = statement(item.node)
            offsets.append(at)
            at += len(line)
            out.write(line)
    tree_path = path + ".tree"
    with open(tree_path, "w") as out:
        subprocess.run([exe, "tree", path], stdout=out, check=False)

    offsets.append(at)
    actual = {}
    misplaced = set()
    current_at = -1
    strays = collections.defaultdict(list)
    errors = collections.defaultdict(int)
    current = None
    with open(tree_path) as dump:
        lines = dump.read().split("\n")
    body = True
    for line in lines[2:]:
        if body and line[:1] == " ":
            if current is not None:
                current.append(normalize(line))
                if outside(line, offsets[current_at], offsets[current_at + 1]):
                    misplaced.add(current_at)
            continue
        if not body and line[:1] == " ":
            named = MODULE_NAME.match(line)
            if named and current is not None:
                current.append("  Name %s %s" % named.groups())
            continue
        current = None
        if line.startswith("!! "):
            at = bisect.bisect_right(offsets, int(line[3:])) - 1
            errors[at] += 1
            continue
        if not body:
            entry = module_entry(line)
            if entry is not None:
                at = bisect.bisect_right(offsets, entry[0]) - 1
                current = actual.get(at)
                if current is not None:
                    current.append(entry[1])
            continue
        if line.startswith("imports "):
            body = False
            continue
        word, _, rest = line.partition(" ")
        where = int(rest.split(" ")[0])
        at = bisect.bisect_right(offsets, where) - 1
        if at < len(items) and at not in actual:
            current = [normalize(line)]
            current_at = at
            actual[at] = current
        else:
            strays[at].append(word)

    codes = {}
    if errors:
        checked = subprocess.run([exe, "check", path], capture_output=True,
                                 text=True, check=False)
        for line in checked.stderr.split("\n"):
            found = ERROR_LINE.search(line)
            if found:
                at = int(found.group(1)) - 1
                codes.setdefault(at, []).append(
                    (found.group(2), found.group(3)))

    failures = []
    for at, item in enumerate(items):
        got = actual.get(at)
        got_text = None if got is None else "\n".join(got)
        wanted = item.node.fails
        if wanted:
            reported = " ".join(code for code, _ in codes.get(at, []))
            if reported != wanted:
                detail = (wanted, reported or "no error")
                failures.append(Failure(item, "refusal", detail, got_text))
            continue
        if at in errors:
            code = codes.get(at, [("TZ????", "")])[0]
            failures.append(Failure(item, "error", code, got_text))
        elif got is None:
            failures.append(Failure(item, "missing", ("", ""), None))
        elif at in strays:
            failures.append(Failure(item, "split", ("", ""), got_text))
        elif got_text != expected_text(item.node):
            failures.append(Failure(item, "tree", ("", ""), got_text))
        elif at in misplaced:
            failures.append(Failure(item, "place", ("", ""), got_text))
    os.remove(path)
    os.remove(tree_path)
    return failures


def first_difference(expected, actual):
    wanted = expected.split("\n")
    got = (actual or "").split("\n")
    for at in range(max(len(wanted), len(got))):
        left = wanted[at].strip() if at < len(wanted) else "<end>"
        right = got[at].strip() if at < len(got) else "<end>"
        if left != right:
            return "want '%s' got '%s'" % (left, right)
    return "same"


def explain(failure, broken_singles, broken_pairs):
    """Is this failure a consequence of a smaller one that already fails?"""
    item = failure.item
    if not item.pairs:
        return False
    for parent, at, child in item.pairs:
        if parent in broken_singles or child in broken_singles:
            return True
    if len(item.pairs) == 1:
        return False
    return any(pair in broken_pairs for pair in item.pairs)


EXPRESSION_LEVELS = ["A", "R1", "L1", "L2", "S2", "M1", "M2"]


def main(enumerate_levels=enumerate_levels, usual=EXPRESSION_LEVELS,
         largest="L3"):
    global BARE
    exe, work = sys.argv[1], sys.argv[2]
    BARE = len(sys.argv) > 4 and sys.argv[4] == "bare"
    levels = list(usual)
    if len(sys.argv) > 3 and sys.argv[3] == "all":
        levels.append(largest)
    elif len(sys.argv) > 3:
        levels = sys.argv[3].split(",")
    os.makedirs(work, exist_ok=True)

    started = time.time()
    seen = set()
    failures = []
    counts = collections.Counter()
    pending = []
    number = 0
    for item in enumerate_levels(levels):
        if item.node.text in seen:
            continue
        if BARE and item.node.text.startswith(NOT_AT_START):
            continue
        seen.add(item.node.text)
        counts[item.level] += 1
        pending.append(item)
        if len(pending) == CHUNK:
            failures.extend(run_chunk(exe, work, number, pending))
            number += 1
            pending = []
            print("  %d checked, %d failed, %.0f s"
                  % (len(seen), len(failures), time.time() - started),
                  flush=True)
    if pending:
        failures.extend(run_chunk(exe, work, number, pending))
    elapsed = time.time() - started

    broken_singles = set()
    broken_pairs = set()
    for failure in failures:
        item = failure.item
        if not item.pairs and not item.node.fails:
            broken_singles.add(item.node.kind)
    for failure in failures:
        item = failure.item
        if len(item.pairs) == 1:
            if not explain(failure, broken_singles, broken_pairs):
                broken_pairs.add(item.pairs[0])

    groups = collections.defaultdict(list)
    explained = 0
    for failure in failures:
        if explain(failure, broken_singles, broken_pairs):
            explained += 1
            continue
        if failure.kind == "error":
            key = "error %s %s" % failure.detail
        elif failure.kind == "refusal":
            key = "must refuse with %s, got: %s" % failure.detail
        elif failure.kind == "tree":
            key = "tree: " + first_difference(
                expected_text(failure.item.node), failure.actual)
        else:
            key = failure.kind
        groups[key].append(failure)

    total = sum(counts.values())
    report = os.path.join(work, "report.txt")
    with open(report, "w", encoding="utf-8") as out:
        out.write("checked %d expressions in %.0f s: %s\n" % (
            total, elapsed,
            ", ".join("%s %d" % pair for pair in sorted(counts.items()))))
        out.write("failed %d, of them %d follow from a smaller failure\n\n"
                  % (len(failures), explained))
        ordered = sorted(groups.items(), key=lambda pair: -len(pair[1]))
        for key, members in ordered:
            out.write("=== %d x %s\n" % (len(members), key))
            by_level = collections.Counter(m.item.level for m in members)
            out.write("    levels: %s\n" % dict(by_level))
            for member_ in members[:12]:
                out.write("    %-4s %-40s %s\n" % (
                    member_.item.level, member_.item.node.desc,
                    member_.item.node.text))
            sample = members[0]
            out.write("  expected:\n%s\n" % expected_text(sample.item.node))
            out.write("  actual:\n%s\n\n" % sample.actual)
        out.write("\n--- all unexplained failures\n")
        for key, members in ordered:
            for member_ in members:
                out.write("%s\t%s\t%s\t%s\n" % (
                    member_.item.level, key, member_.item.node.desc,
                    member_.item.node.text))
    print("checked %d in %.0f s, failed %d (%d explained), groups %d"
          % (total, elapsed, len(failures), explained, len(groups)))
    for key, members in ordered[:20]:
        print("  %d x %s: %s" % (len(members), key,
                                 members[0].item.node.text))
    print("report: " + report)
    if failures:
        sys.exit(1)


if __name__ == "__main__":
    main()
