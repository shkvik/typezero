"""Enumerates tz statements together with the trees they must give and checks
the self/ parser against them.  Works on top of exprgen.py: same runner, same
report, and the expressions inside the statements come from there.

    python stmtgen.py <tzparse.exe> <work dir> [levels|all]

levels: A statements without parts, R1 every statement with plain parts,
E1 every expression in every place of an expression, M1 every expression
the parser must refuse in those places, S2 every statement in every place of
a statement, P2 two statements side by side, SE and PE the same with the
statements of E1, S3 three statements deep (only with 'all').

Only valid programs are built: 'return' stays inside a function, 'break'
and 'continue' inside a loop, a declaration is never the single statement
of 'if' or of a loop, and an 'else' is never written where it would join
another 'if'.

Where tz knowingly differs from ECMAScript the generator follows tz:
  - 'for...of' and 'for...in' declare a new variable; 'for (a of b)' is not
    generated;
  - the value of 'case' has no comma; 'case a, b:' is not generated.

An assignment to a pattern is a statement of its own ('[v, w] = a;',
'({ v, w } = a);'), and the tree holds what it is rewritten to: a block with
the value in a hidden constant and one assignment for every name; a nested
pattern is a block inside.  When the value is an array literal with a value
for every name and neither it nor the pattern has '...' or a hole, each of
its values gets a hidden constant of its own ('[v, w] = [w, v]' swaps).

'using' declares like 'const' and always has a value; 'for (using a of b)'
and 'await using' are not generated: tz has neither.

A class without a name that is the value of a variable gets the name of the
variable in the tree ('const v = class { }' is the class "v"): that is how
the parser writes it down, and the builder of the declaration does the same.
"""

import collections

import exprgen as E
from exprgen import ASSIGN, COMMA, S, wrap

MISSING = E.MISSING
INT_TYPE = E.INT_TYPE
LOOP_EXITS = frozenset(["break", "continue"])


class Stmt:
    __slots__ = ("text", "shape", "kind", "desc", "free", "decl", "open_if",
                 "fails", "kids", "line", "block")

    def __init__(self, text, shape, kind, free=frozenset(), decl=False,
                 open_if=False):
        self.text = text
        self.shape = shape
        self.kind = kind
        self.desc = kind
        self.free = frozenset(free)
        self.decl = decl
        self.open_if = open_if
        self.fails = None
        self.kids = ()
        self.line = None
        self.block = None


def needs(part):
    """What must be around this part: a function, a loop, a generator."""
    if isinstance(part, Stmt):
        return part.free
    found = set()
    if "yield" in part.text:
        found.add("yield")
    if "await " in part.text:
        found.add("await")
    return frozenset(found)


def all_needs(*parts):
    found = frozenset()
    for part in parts:
        found |= needs(part)
    return found


def written(expr, least=COMMA):
    return wrap(expr, least)[0]


def name_shape(word):
    return S('Name "%s" targs=0' % word)


def declarator(word, init, type_=MISSING):
    return S('Declarator "%s" form=plain' % word, type_, init)


def named_after(word, shape):
    """The tree of a value of a variable: a class without a name takes the
    name of the variable."""
    if shape[0] != "ClassExpr" or not shape[1][0][0].startswith('Class ""'):
        return shape
    line, parts = shape[1][0]
    named = line.replace('Class ""', 'Class "%s"' % word, 1)
    return S("ClassExpr", (named, parts))


# ------------------------------------------------------------ statements

def expression(expr):
    text = expr.text
    if text.startswith(E.NOT_AT_START):
        text = "(" + text + ")"
    return Stmt(text + ";", S("ExprStmt", expr.shape), "x;", needs(expr))


def variable(keyword, typed):
    def build(init):
        note = ": int" if typed else ""
        text = "%s v%s = %s;" % (keyword, note, written(init, ASSIGN))
        type_ = INT_TYPE if typed else MISSING
        shape = S("VarDecl const=%d using=0 n=1" % (keyword == "const"),
                  declarator("v", named_after("v", init.shape), type_))
        return Stmt(text, shape, keyword, needs(init), decl=True)
    return build


def using(typed):
    def build(init):
        note = ": int" if typed else ""
        text = "using v%s = %s;" % (note, written(init, ASSIGN))
        type_ = INT_TYPE if typed else MISSING
        shape = S("VarDecl const=1 using=1 n=1",
                  declarator("v", named_after("v", init.shape), type_))
        return Stmt(text, shape, "using", needs(init), decl=True)
    return build


def two_usings(first, second):
    text = "using v = %s, w = %s;" % (written(first, ASSIGN),
                                      written(second, ASSIGN))
    shape = S("VarDecl const=1 using=1 n=2",
              declarator("v", named_after("v", first.shape)),
              declarator("w", named_after("w", second.shape)))
    return Stmt(text, shape, "using2", all_needs(first, second), decl=True)


def two_variables(first, second):
    text = "let v = %s, w = %s;" % (written(first, ASSIGN),
                                    written(second, ASSIGN))
    shape = S("VarDecl const=0 using=0 n=2",
              declarator("v", named_after("v", first.shape)),
              declarator("w", named_after("w", second.shape)))
    return Stmt(text, shape, "let2", all_needs(first, second), decl=True)


TEMPORARY = name_shape("%")


def element(at):
    return S("Index opt=0", TEMPORARY, S('IntLit %d ""' % at))


def field(word):
    return S('Member "%s" opt=0 targs=0' % word, TEMPORARY)


def array_pattern(rest):
    def build(init):
        second = element(1)
        if rest:
            second = S("Call opt=0 tagged=0 n=1 spread=-",
                       S('Member "slice" opt=0 targs=0', TEMPORARY),
                       S('IntLit 1 ""'))
        text = "const [v, %sw] = %s;" % ("..." if rest else "",
                                         written(init, ASSIGN))
        shape = S("VarDecl const=1 using=0 n=3",
                  S('Declarator "%%" form=array 2 %d' % rest, MISSING,
                    init.shape),
                  declarator("v", element(0)), declarator("w", second))
        return Stmt(text, shape, "const[]", needs(init), decl=True)
    return build


def object_pattern(init):
    text = "const { v, w } = %s;" % written(init, ASSIGN)
    shape = S("VarDecl const=1 using=0 n=3",
              S('Declarator "%" form=object', MISSING, init.shape),
              declarator("v", field("v")), declarator("w", field("w")))
    return Stmt(text, shape, "const{}", needs(init), decl=True)


def object_pattern_default(default, init):
    text = "const { k: v, w = %s } = %s;" % (written(default, ASSIGN),
                                             written(init, ASSIGN))
    shape = S("VarDecl const=1 using=0 n=3",
              S('Declarator "%" form=object', MISSING, init.shape),
              declarator("v", field("k")),
              declarator("w", S("Binary 20", field("w"), default.shape)))
    return Stmt(text, shape, "const{=}", all_needs(default, init), decl=True)


# --------------------------------------------- assignments to a pattern

def assigned(target, value):
    return S("ExprStmt", S("Assign -", target, value))


def hidden(form, init_shape):
    return S("VarDecl const=1 using=0 n=1",
             S('Declarator "%%" form=%s' % form, MISSING, init_shape))


def rewritten(form, init_shape, *parts):
    return S("Block n=%d" % (1 + len(parts)), hidden(form, init_shape),
             *parts)


def or_default(read, default):
    return S("Binary 20", read, default.shape)


def literal_value(init):
    return init.shape[0].startswith("ArrayLit")


def array_assignment(kind):
    v, w = name_shape("v"), name_shape("w")
    rest_of = S("Call opt=0 tagged=0 n=1 spread=-",
                S('Member "slice" opt=0 targs=0', TEMPORARY),
                S('IntLit 1 ""'))
    inner = rewritten("array 2 0", element(1),
                      assigned(w, element(0)),
                      assigned(name_shape("u"), element(1)))
    forms = {
        "[]=": ("[v, w]", "array 2 0",
                [assigned(v, element(0)), assigned(w, element(1))]),
        "[...]=": ("[v, ...w]", "array 2 1",
                   [assigned(v, element(0)), assigned(w, rest_of)]),
        "[,]=": ("[, w]", "array 2 0", [assigned(w, element(1))]),
        "[.x]=": ("[v.x, w[0]]", "array 2 0",
                  [assigned(S('Member "x" opt=0 targs=0', v), element(0)),
                   assigned(S("Index opt=0", w, S('IntLit 0 "0"')),
                            element(1))]),
        "[[]]=": ("[v, [w, u]]", "array 2 0",
                  [assigned(v, element(0)), inner]),
    }
    pattern, form, parts = forms[kind]

    def build(init):
        if literal_value(init) and kind != "[...]=":
            return None
        text = "%s = %s;" % (pattern, written(init, ASSIGN))
        return Stmt(text, rewritten(form, init.shape, *parts), kind,
                    needs(init))
    return build


def array_assignment_default(default, init):
    if literal_value(init):
        return None
    text = "[v = %s, w] = %s;" % (written(default, ASSIGN),
                                  written(init, ASSIGN))
    shape = rewritten("array 2 0", init.shape,
                      assigned(name_shape("v"),
                               or_default(element(0), default)),
                      assigned(name_shape("w"), element(1)))
    return Stmt(text, shape, "[=]=", all_needs(default, init))


def array_assignment_direct(first, second):
    text = "[v, w] = [%s, %s];" % (written(first, ASSIGN),
                                   written(second, ASSIGN))
    kept = S("VarDecl const=1 using=0 n=2",
             S('Declarator "%" form=plain', MISSING, first.shape),
             S('Declarator "%" form=plain', MISSING, second.shape))
    shape = S("Block n=3", kept, assigned(name_shape("v"), TEMPORARY),
              assigned(name_shape("w"), TEMPORARY))
    return Stmt(text, shape, "[]=[]", all_needs(first, second))


def object_assignment(kind):
    v, w = name_shape("v"), name_shape("w")
    others = S("ObjectLit n=1",
               S('Property "" Spread omit=["v"] keys=[]', TEMPORARY))
    inner = S("Block n=2", hidden("object", field("k")),
              assigned(v, field("v")))
    forms = {
        "{}=": ("{ v, w }", [assigned(v, field("v")),
                             assigned(w, field("w"))]),
        "{...}=": ("{ v, ...w }", [assigned(v, field("v")),
                                   assigned(w, others)]),
        "{{}}=": ("{ k: { v }, w }", [inner, assigned(w, field("w"))]),
    }
    pattern, parts = forms[kind]

    def build(init):
        text = "(%s = %s);" % (pattern, written(init, ASSIGN))
        return Stmt(text, rewritten("object", init.shape, *parts), kind,
                    needs(init))
    return build


def object_assignment_default(default, init):
    text = "({ k: v, w = %s } = %s);" % (written(default, ASSIGN),
                                         written(init, ASSIGN))
    shape = rewritten("object", init.shape,
                      assigned(name_shape("v"), field("k")),
                      assigned(name_shape("w"),
                               or_default(field("w"), default)))
    return Stmt(text, shape, "{=}=", all_needs(default, init))


def block(*items):
    text = "{ " + " ".join(item.text for item in items) + " }"
    shape = S("Block n=%d" % len(items), *[item.shape for item in items])
    return Stmt(text, shape, "{}", all_needs(*items))


def if_(cond, then):
    text = "if (%s) %s" % (written(cond), then.text)
    return Stmt(text, S("If", cond.shape, then.shape, MISSING), "if",
                all_needs(cond, then), open_if=True)


def if_else(cond, then, other):
    if then.open_if:
        return None
    text = "if (%s) %s else %s" % (written(cond), then.text, other.text)
    return Stmt(text, S("If", cond.shape, then.shape, other.shape), "if else",
                all_needs(cond, then, other), open_if=other.open_if)


def inside_loop(body, *around):
    return (needs(body) - LOOP_EXITS) | all_needs(*around)


def while_(cond, body):
    text = "while (%s) %s" % (written(cond), body.text)
    return Stmt(text, S("While", cond.shape, body.shape), "while",
                inside_loop(body, cond), open_if=body.open_if)


def do_while(body, cond):
    text = "do %s while (%s);" % (body.text, written(cond))
    return Stmt(text, S("DoWhile", body.shape, cond.shape), "do",
                inside_loop(body, cond))


def without_in(expr, least=COMMA):
    text = written(expr, least)
    if " in " in text:
        text = "(" + text + ")"
    return text


def for_empty(body):
    text = "for (;;) " + body.text
    shape = S("For", MISSING, MISSING, MISSING, body.shape)
    return Stmt(text, shape, "for(;;)", inside_loop(body),
                open_if=body.open_if)


def for_three(declares):
    def build(init, cond, step, body):
        if declares:
            head = "let i = " + without_in(init, ASSIGN)
            first = S("VarDecl const=0 using=0 n=1",
                      declarator("i", init.shape))
        else:
            head = without_in(init)
            first = S("ExprStmt", init.shape)
        text = "for (%s; %s; %s) %s" % (head, written(cond), written(step),
                                        body.text)
        shape = S("For", first, cond.shape, step.shape, body.shape)
        kind = "for(let;;)" if declares else "for(x;;)"
        return Stmt(text, shape, kind, inside_loop(body, init, cond, step),
                    open_if=body.open_if)
    return build


def for_of(keyword, word, is_await):
    def build(iterable, body):
        wait = "await " if is_await else ""
        text = "for %s(%s v %s %s) %s" % (wait, keyword, word,
                                          written(iterable, ASSIGN), body.text)
        label = "ForOf const=%d keys=%d await=%d" % (
            keyword == "const", word == "in", is_await)
        shape = S(label, declarator("v", MISSING), iterable.shape, body.shape)
        free = inside_loop(body, iterable)
        if is_await:
            free |= frozenset(["await"])
        kind = "for %s%s %s" % (wait, keyword, word)
        return Stmt(text, shape, kind, free, open_if=body.open_if)
    return build


def for_of_pattern(iterable, body):
    text = "for (const [v, w] of %s) %s" % (written(iterable, ASSIGN),
                                            body.text)
    parts = S("VarDecl const=1 using=0 n=2", declarator("v", element(0)),
              declarator("w", element(1)))
    shape = S("ForOf const=1 keys=0 await=0",
              S('Declarator "%" form=array 0 0', MISSING, MISSING),
              iterable.shape, S("Block n=2", parts, body.shape))
    return Stmt(text, shape, "for const[] of", inside_loop(body, iterable),
                open_if=body.open_if)


def inside_switch(*parts):
    return all_needs(*parts) - frozenset(["break"])


def switch_empty(subject):
    text = "switch (%s) { }" % written(subject)
    return Stmt(text, S("Switch n=0", subject.shape), "switch{}",
                needs(subject))


def switch_case(subject, test, item):
    text = "switch (%s) { case %s: %s }" % (
        written(subject), written(test, ASSIGN), item.text)
    shape = S("Switch n=1", subject.shape,
              S("Case n=1", test.shape, item.shape))
    return Stmt(text, shape, "switch case",
                inside_switch(item) | all_needs(subject, test))


def switch_two(subject, test, first, second):
    text = "switch (%s) { case %s: %s break; default: %s }" % (
        written(subject), written(test, ASSIGN), first.text, second.text)
    shape = S("Switch n=2", subject.shape,
              S("Case n=2", test.shape, first.shape, S('Break ""')),
              S("Case n=1", MISSING, second.shape))
    return Stmt(text, shape, "switch case default",
                inside_switch(first, second) | all_needs(subject, test))


def switch_default_first(subject, item, test):
    text = "switch (%s) { default: %s case %s: }" % (
        written(subject), item.text, written(test, ASSIGN))
    shape = S("Switch n=2", subject.shape,
              S("Case n=1", MISSING, item.shape), S("Case n=0", test.shape))
    return Stmt(text, shape, "switch default case",
                inside_switch(item) | all_needs(subject, test))


def labeled(body):
    if "l:" in body.text:
        return None
    return Stmt("l: " + body.text, S('Labeled "l"', body.shape), "l:",
                needs(body), open_if=body.open_if)


def return_(value):
    free = needs(value) | frozenset(["return"])
    return Stmt("return %s;" % written(value), S("Return", value.shape),
                "return x", free)


def function_line(word, generator, is_async, params):
    return ('Function "%s" arrow=0 concise=0 own="" place=0 body=0 gen=%d '
            'async=%d role=plain tp=0 params=%d'
            % (word, generator, is_async, params))


def function(generator, is_async, params):
    def build(item):
        free = needs(item)
        if free & LOOP_EXITS:
            return None
        if "yield" in free and not generator:
            return None
        if "await" in free and not is_async:
            return None
        head = "%sfunction%s f(%s)" % ("async " if is_async else "",
                                       "*" if generator else "",
                                       ", ".join(params))
        listed = [E.parameter(word) for word in params]
        shape = S(function_line("f", generator, is_async, len(params)),
                  *listed, MISSING, S("Block n=1", item.shape))
        kind = head.replace(" f", "")
        return Stmt("%s { %s }" % (head, item.text), shape, kind, decl=True)
    return build


def atoms():
    def plain(text, label, kind, free=()):
        return Stmt(text, S(label), kind, free)

    exit_by_label = S('Labeled "l"',
                      S("While", name_shape("a"),
                        S("Block n=1", S('Break "l"'))))
    next_by_label = S('Labeled "l"',
                      S("For", MISSING, MISSING, MISSING,
                        S("Block n=1", S('Continue "l"'))))
    typed = Stmt("let v: int;", S("VarDecl const=0 using=0 n=1",
                                  declarator("v", MISSING, INT_TYPE)),
                 "let:", decl=True)
    return [
        plain(";", "Empty", ";"),
        plain("debugger;", "Debugger", "debugger"),
        plain("{ }", "Block n=0", "{ }"),
        plain("break;", 'Break ""', "break", ["break"]),
        plain("continue;", 'Continue ""', "continue", ["continue"]),
        Stmt("return;", S("Return", MISSING), "return", ["return"]),
        Stmt("l: while (a) { break l; }", exit_by_label, "break l"),
        Stmt("l: for (;;) { continue l; }", next_by_label, "continue l"),
        typed,
    ]


Construct = collections.namedtuple("Construct", "name slots build")


def constructs():
    made = []

    def add(label, slots, build):
        made.append(Construct(label, slots, build))

    add("x;", ["expr"], expression)
    add("const", ["expr"], variable("const", False))
    add("let", ["expr"], variable("let", False))
    add("const:", ["expr"], variable("const", True))
    add("let2", ["expr", "expr"], two_variables)
    add("const[]", ["expr"], array_pattern(False))
    add("const[...]", ["expr"], array_pattern(True))
    add("const{}", ["expr"], object_pattern)
    add("const{=}", ["expr", "expr"], object_pattern_default)
    add("using", ["expr"], using(False))
    add("using:", ["expr"], using(True))
    add("using2", ["expr", "expr"], two_usings)
    for kind in ("[]=", "[...]=", "[,]=", "[.x]=", "[[]]="):
        add(kind, ["expr"], array_assignment(kind))
    add("[=]=", ["expr", "expr"], array_assignment_default)
    add("[]=[]", ["expr", "expr"], array_assignment_direct)
    for kind in ("{}=", "{...}=", "{{}}="):
        add(kind, ["expr"], object_assignment(kind))
    add("{=}=", ["expr", "expr"], object_assignment_default)
    add("{x}", ["item"], block)
    add("{x y}", ["item", "item"], block)
    add("if", ["expr", "body"], if_)
    add("if else", ["expr", "body", "body"], if_else)
    add("while", ["expr", "body"], while_)
    add("do", ["body", "expr"], do_while)
    add("for(;;)", ["body"], for_empty)
    add("for(x;;)", ["expr", "expr", "expr", "body"], for_three(False))
    add("for(let;;)", ["expr", "expr", "expr", "body"], for_three(True))
    add("for const of", ["expr", "body"], for_of("const", "of", False))
    add("for let of", ["expr", "body"], for_of("let", "of", False))
    add("for const in", ["expr", "body"], for_of("const", "in", False))
    add("for await", ["expr", "body"], for_of("const", "of", True))
    add("for const[] of", ["expr", "body"], for_of_pattern)
    add("switch{}", ["expr"], switch_empty)
    add("switch case", ["expr", "expr", "item"], switch_case)
    add("switch case default", ["expr", "expr", "item", "item"], switch_two)
    add("switch default case", ["expr", "item", "expr"], switch_default_first)
    add("l:", ["body"], labeled)
    add("return x", ["expr"], return_)
    add("function", ["item"], function(False, False, []))
    add("function(p,q)", ["item"], function(False, False, ["p", "q"]))
    add("async function", ["item"], function(False, True, []))
    add("function*", ["item"], function(True, False, []))
    return made


def accepts(slot, part):
    if slot == "expr":
        return isinstance(part, E.Node)
    if not isinstance(part, Stmt):
        return False
    return slot == "item" or not part.decl


def plain_part(slot, word):
    if slot == "expr":
        return E.name(word)
    return expression(E.name(word))


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


# ----------------------------------------------------------- whole lines

def lines_of(shape):
    out = []
    E.flatten(shape, 0, out)
    return "\n".join(out)


def ready(stmt):
    """Puts the statement where it is allowed and writes down its line of
    source and the lines of its tree."""
    text, shape, free = stmt.text, stmt.shape, stmt.free
    if free & LOOP_EXITS:
        text = "while (w) { %s }" % text
        shape = S("While", name_shape("w"), S("Block n=1", shape))
    if "return" in free or "yield" in free:
        generator = "yield" in free
        is_async = "await" in free
        text = "%sfunction%s f() { %s }" % ("async " if is_async else "",
                                            "*" if generator else "", text)
        shape = S(function_line("f", generator, is_async, 0), MISSING,
                  S("Block n=1", shape))
    stmt.line = text + "\n"
    stmt.block = lines_of(shape)
    return stmt


# ----------------------------------------------------------- enumeration

def expression_parts():
    parts = list(E.atoms())
    for construct in E.constructs():
        node = E.fill(construct, {}, "abc")
        if node is not None:
            parts.append(node)
    return parts


def deepen(made, inner, slots, words, level):
    """Every construct with one of the inner parts in each place of the
    given kinds."""
    for construct in made:
        for at, slot in enumerate(construct.slots):
            if slot not in slots:
                continue
            for part in inner:
                stmt = fill(construct, {at: part.node}, words)
                if stmt is None:
                    continue
                pair = (construct.name, at, part.node.kind)
                yield E.Item(level, (pair,) + part.pairs, ready(stmt))


def side_by_side(firsts, seconds, level):
    two = Construct("{x y}", ["item", "item"], block)
    for first in firsts:
        for second in seconds:
            stmt = fill(two, {0: first.node, 1: second.node}, "ab")
            if stmt is None:
                continue
            pairs = (("{x y}", 0, first.node.kind),
                     ("{x y}", 1, second.node.kind))
            yield E.Item(level, pairs, ready(stmt))


STATEMENTS = ("body", "item")


def enumerate_levels(levels):
    made = constructs()
    exprs = [E.Item("E", (), node) for node in expression_parts()]
    wrong = [E.Item("M", (), node) for node in E.mistakes()]
    alone = [E.Item("A", (), ready(stmt)) for stmt in atoms()]
    plain = list(alone)
    for construct in made:
        stmt = fill(construct, {}, "abcd")
        plain.append(E.Item("R1", (), ready(stmt)))
    if "A" in levels:
        yield from alone
    if "R1" in levels:
        yield from plain[len(alone):]
    with_exprs = list(deepen(made, exprs, ("expr",), "abcd", "E1"))
    if "E1" in levels:
        yield from with_exprs
    if "M1" in levels:
        yield from deepen(made, wrong, ("expr",), "abcd", "M1")
    nested = list(deepen(made, plain, STATEMENTS, "wxyz", "S2"))
    if "S2" in levels:
        yield from nested
    if "P2" in levels:
        yield from side_by_side(plain, plain, "P2")
    if "SE" in levels:
        yield from deepen(made, with_exprs, STATEMENTS, "wxyz", "SE")
    if "PE" in levels:
        yield from side_by_side(plain, with_exprs, "PE")
    if "S3" in levels:
        yield from deepen(made, nested, STATEMENTS, "stuv", "S3")


if __name__ == "__main__":
    E.main(enumerate_levels,
           ["A", "R1", "E1", "M1", "S2", "P2", "SE", "PE"], "S3")
