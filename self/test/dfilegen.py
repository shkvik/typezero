feat(self): using in loop heads, unnamed class error, tests in self/test
"""Declaration files: what X.d.tz next to its module X.tz may hold.

    python dfilegen.py <tzparse.exe> <work dir>

tzparse reads X.d.tz as a declaration file only when X.tz is next to it, so
every file written here gets an empty module beside it.

Three parts:
  1. correct items with the tree the parser must build, positions removed;
     the place of every class and member name is checked apart;
  2. mistakes, one per file between two correct items: exactly one error,
     with the expected code;
  3. one token of a correct one-line item is spoiled (as mutgen does): the
     parser neither fails nor hangs; lines with more than one error are
     counted.
The script exits with 1 when a tree or a place is wrong, a mistake does not
give its one error, or the parser fails or hangs.

The rules come from docs/modules.md, section 7, and for the modifiers of a
member from the TypeScript grammar; only the shape of the dump is taken from
the dumper.
  - The top level holds imports, 'export type', 'export interface', 'export
    declare' of a function, an [abstract] class, an enum, a const or a let,
    'export { ... }' and 'export *'.  Nothing else: no code.
  - A declared function and a declared method end after the result type; a
    declared field and a declared const or let end after the type, which
    they must have: there is no value.  A declared enum lists its members
    with their values, as TypeScript does (decided 17.11).
  - A declared class lists members only: no 'extends', no 'implements'.
  - Modifiers stand in the order of TypeScript: access, then 'static' or
    'abstract', then 'override', then 'readonly' or 'async'.  'readonly' is
    for fields, 'async' for methods, 'abstract' for the methods of an
    abstract class (and not next to 'async'), a constructor takes access
    only.  'async' and 'override' say nothing in a declaration: the parser
    takes them and the tree does not keep them (decided 17.11).
  - A modifier word followed by '(', ':', '?', ';' or '<' is the name of the
    member, not a modifier.
"""

import collections
import itertools
import os
import re
import subprocess
import sys

import mutgen as Mu

LIMIT_SECONDS = 600


def tree(line, *children):
    return (line, list(children))


MISSING = tree("-")


def render(node, depth=0):
    line, children = node
    lines = ["  " * depth + line]
    for child in children:
        lines.extend(render(child, depth + 1))
    return lines


def named(word):
    return tree('Type Name "%s" n=0' % word)


INT = named("int")
STRING = named("string")
INT_ARRAY = tree('Type Array "" n=1', INT)
BOX = tree('Type Generic "Box" n=1', INT)
TYPES = [
    ("int", INT),
    ("string", STRING),
    ("int[]", INT_ARRAY),
    ("Box<int>", BOX),
    ("int | undefined", tree('Type Union "" n=2', INT, named("undefined"))),
    ("{ a: int }",
     tree('Type Object "" n=1 names=["a"] opt=0 ro=0 methods=-', INT)),
]


def param(word, form, type_):
    return tree('Param "%s" form=%d prop=- dec=0' % (word, form), type_)


def type_param(word, limit=MISSING):
    return tree('TypeParam "%s"' % word, limit, MISSING)


# text, type parameters, parameters
SIGNATURES = [
    ("()", [], []),
    ("(a: int)", [], [param("a", 0, INT)]),
    ("(a: int, b?: string, ...c: int[])", [],
     [param("a", 0, INT), param("b", 1, STRING), param("c", 3, INT_ARRAY)]),
    ("<U>(a: U)", [type_param("U")], [param("a", 0, named("U"))]),
    ("<U extends Box<int>, V>(a: U, b: V)",
     [type_param("U", BOX), type_param("V")],
     [param("a", 0, named("U")), param("b", 0, named("V"))]),
]
RESULTS = [("", MISSING), (": void", named("void")), (": int[]", INT_ARRAY)]

# word, access, written
ACCESS = [("", 0, 0), ("public", 0, 1), ("protected", 1, 1), ("private", 2, 1)]

# text, name in the tree
NAMES = [
    ("value", "value"),
    ("delete", "delete"),
    ("type", "type"),
    ("static", "static"),
    ("readonly", "readonly"),
    ("public", "public"),
    ("abstract", "abstract"),
    ("async", "async"),
    ("override", "override"),
    ("declare", "declare"),
    ("accessor", "accessor"),
    ("get", "get"),
    ("set", "set"),
    ('"content-type"', "content-type"),
    ("[Symbol.iterator]", "[Symbol.iterator]"),
    ("[Symbol.asyncIterator]", "[Symbol.asyncIterator]"),
    ("[Symbol.dispose]", "[Symbol.dispose]"),
]

Case = collections.namedtuple("Case", "sort text expected names")
# sort: item | class | import | export; names: offsets of the class name and
# of every member name in the text


def item(text, expected):
    return Case("item", text, render(expected), [])


def declared(word, access, written, static, form, children):
    line = 'Declared "%s" access=%d written=%d static=%d %s' % (
        word, access, written, static, form)
    return tree(line, *children)


def class_case(members, abstract=0, type_params=(), type_text="",
               one_line=False):
    """members: [(text before the name, name text, text after, tree)]"""
    head = "export declare %sclass C%s {" % ("abstract " * abstract, type_text)
    names = [head.index("class C") + 6]
    text = head
    for before, word, after, _ in members:
        text += " " if one_line else "\n  "
        text += before
        names.append(len(text))
        text += word + after
    text += " }" if one_line else "\n}"
    if not members:
        text = head + "}"
    line = 'DeclaredClass "C" abstract=%d tp=%d members=%d' % (
        abstract, len(type_params), len(members))
    expected = tree(line, *type_params, *[member[3] for member in members])
    return Case("class", text, render(expected), names)


def words(*parts):
    return "".join(part + " " for part in parts if part)


def fields():
    for access_word, access, written in ACCESS:
        for static, override, readonly in itertools.product((0, 1), repeat=3):
            before = words(access_word, "static" * static,
                           "override" * override, "readonly" * readonly)
            for text, word in NAMES:
                for optional in (0, 1):
                    form = "field ro=%d optional=%d" % (readonly, optional)
                    shape = declared(word, access, written, static, form,
                                     [INT])
                    after = "?" * optional + ": int;"
                    yield before, text, after, shape
    for type_text, type_shape in TYPES:
        shape = declared("value", 0, 0, 0, "field ro=0 optional=0",
                         [type_shape])
        yield "", "value", ": %s;" % type_text, shape


def methods():
    """(member, needs an abstract class)"""
    for access_word, access, written in ACCESS:
        for kind, override, is_async in itertools.product(
                ("", "static", "abstract"), (0, 1), (0, 1)):
            if kind == "abstract" and is_async:
                continue
            before = words(access_word, kind, "override" * override,
                           "async" * is_async)
            for text, word in NAMES:
                for at, (signature, type_params, params) in enumerate(
                        SIGNATURES):
                    result_text, result = RESULTS[at % len(RESULTS)]
                    form = "method tp=%d params=%d" % (len(type_params),
                                                       len(params))
                    shape = declared(word, access, written, kind == "static",
                                     form, type_params + params + [result])
                    after = signature + result_text + ";"
                    yield (before, text, after, shape), kind == "abstract"


def constructors():
    for access_word, access, written in ACCESS:
        for signature, type_params, params in SIGNATURES[:3]:
            form = "constructor tp=0 params=%d" % len(params)
            shape = declared("constructor", access, written, 0, form,
                             params + [MISSING])
            yield words(access_word), "constructor", signature + ";", shape


def function(word, signature, result, generator=0):
    text, type_params, params = signature
    result_text, result_shape = result
    line = ('Function "%s" arrow=0 concise=0 own="" place=0 body=0 gen=%d '
            'async=0 role=plain tp=%d params=%d'
            % (word, generator, len(type_params), len(params)))
    shape = tree(line, *type_params, *params, result_shape, MISSING)
    star = "*" * generator
    return item("export declare function%s %s%s%s;" % (star, word, text,
                                                       result_text), shape)


def enum_member(word, value=MISSING):
    return tree('EnumMember "%s"' % word, value)


def variable(word, type_shape):
    return tree('Declarator "%s" form=plain' % word, type_shape, MISSING)


def value_cases():
    one = tree('IntLit 1 "1"')
    text = tree('StringLit "s"')
    enums = [
        ("{ }", []),
        ("{ A }", [enum_member("A")]),
        ("{ A, B }", [enum_member("A"), enum_member("B")]),
        ("{ A, B, }", [enum_member("A"), enum_member("B")]),
        ('{ A = 1, B = "s" }', [enum_member("A", one),
                                enum_member("B", text)]),
    ]
    for body, members in enums:
        shape = tree('Enum "E" n=%d' % len(members), *members)
        yield item("export declare enum E %s" % body, shape)
        yield item("export declare const enum E %s" % body, shape)
    for word, const in (("const", 1), ("let", 0)):
        line = "VarDecl const=%d using=0 n=%%d" % const
        for type_text, type_shape in TYPES:
            yield item("export declare %s a: %s;" % (word, type_text),
                       tree(line % 1, variable("a", type_shape)))
        yield item("export declare %s a: int, b: string;" % word,
                   tree(line % 2, variable("a", INT), variable("b", STRING)))


def module_name(word, alias):
    return '  Name "%s" "%s"' % (word, alias)


def module_cases():
    path = '"./a.tz"'
    yield Case("import", 'import { A } from %s;' % path,
               ['Import %s "" 0 0 1' % path, module_name("A", "A")], [])
    yield Case("import", 'import { A as B, C } from %s;' % path,
               ['Import %s "" 0 0 2' % path, module_name("A", "B"),
                module_name("C", "C")], [])
    yield Case("export", "export { A };",
               ['Export 0 "" 1', module_name("A", "A")], [])
    yield Case("export", "export { A as B, C };",
               ['Export 0 "" 2', module_name("A", "B"),
                module_name("C", "C")], [])
    yield Case("export", "export { A as B } from %s;" % path,
               ['Export 1 %s 1' % path, module_name("A", "B")], [])
    yield Case("export", "export * from %s;" % path,
               ['Export 2 %s 0' % path], [])


def correct_cases():
    alias = 'TypeAlias "%s" tp=0 bases=0'
    for type_text, type_shape in TYPES:
        yield item("export type A = %s;" % type_text,
                   tree(alias % "A", type_shape))
    object_text, object_shape = TYPES[-1]
    yield item("export interface I %s" % object_text,
               tree(alias % "I", object_shape))
    for signature in SIGNATURES:
        for result in RESULTS:
            yield function("f", signature, result)
    yield function("g", SIGNATURES[0], RESULTS[2], generator=1)
    yield from value_cases()
    yield from module_cases()

    yield class_case([])
    yield class_case([], abstract=1)
    yield class_case([], type_params=[type_param("T")], type_text="<T>")
    yield class_case([], abstract=1,
                     type_params=[type_param("T", BOX), type_param("U")],
                     type_text="<T extends Box<int>, U>")
    for member in fields():
        yield class_case([member])
    for member, abstract in methods():
        yield class_case([member], abstract=int(abstract))
    for member in constructors():
        yield class_case([member])
    some_fields = list(fields())[:3]
    some_methods = [member for member, abstract in methods()
                    if not abstract][:3]
    several = some_fields + list(constructors())[:1] + some_methods
    yield class_case(several)
    yield class_case(several, one_line=True)


BEFORE = "export type Before = int;"
AFTER = "export type After = int;"


def in_class(member):
    return "export declare class C {\n  %s\n}" % member


MISTAKES = [
    ("const a = 1;", "TZ1701"),
    ("let a: int;", "TZ1701"),
    ("function f(): void {}", "TZ1701"),
    ("class A {}", "TZ1701"),
    ("type A = int;", "TZ1701"),
    ("interface I { a: int }", "TZ1701"),
    ("declare function f(): void;", "TZ1701"),
    ("declare class A {}", "TZ1701"),
    ("enum E { A }", "TZ1701"),
    ("a();", "TZ1701"),
    ("if (a) { b(); }", "TZ1701"),
    ("export const a = 1;", "TZ1702"),
    ("export let a: int;", "TZ1702"),
    ("export function f(): void {}", "TZ1702"),
    ("export function f(): void;", "TZ1702"),
    ("export async function f(): Promise<void> {}", "TZ1702"),
    ("export class A {}", "TZ1702"),
    ("export abstract class A {}", "TZ1702"),
    ("export enum E { A }", "TZ1702"),
    ("export default 1;", "TZ1702"),
    ("export declare var a: int;", "TZ1703"),
    ("export declare const a = 1;", "TZ1708"),
    ("export declare const a: int = 1;", "TZ1708"),
    ("export declare let a: int = 1;", "TZ1708"),
    ("export declare const a;", "TZ1709"),
    ("export declare let a;", "TZ1709"),
    ("export declare enum { A }", "TZ1532"),
    ("export declare const [a, b]: int[];", "TZ1136"),
    ("export declare type A = int;", "TZ1703"),
    ("export declare interface I { a: int }", "TZ1703"),
    ("export declare namespace N {}", "TZ1703"),
    ("export declare async function f(): Promise<void>;", "TZ1703"),
    ("export declare abstract function f(): void;", "TZ1703"),
    ("export declare;", "TZ1703"),
    ("export declare function f(): void {}", "TZ1704"),
    ("export declare function f(a: int): int { return a; }", "TZ1704"),
    ("export declare class A extends B {}", "TZ1705"),
    ("export declare class A implements I {}", "TZ1705"),
    ("export declare class A extends B implements I, J {}", "TZ1705"),
    ("export declare abstract class A<T> extends B<T> {}", "TZ1705"),
    (in_class("f(): void {}"), "TZ1706"),
    (in_class("public static f(a: int): int { return a; }"), "TZ1706"),
    (in_class("constructor(a: int) { this.a = a; }"), "TZ1706"),
    (in_class("a: int = 1;"), "TZ1707"),
    (in_class('public static readonly a: string = "s";'), "TZ1707"),
    ("export declare class {}", "TZ1505"),
    ("export declare abstract class {}", "TZ1505"),
    ("export declare class A;", "TZ1501"),
    ("export type = int;", "TZ1640"),
    ("export interface { a: int }", "TZ1555"),
    ("export type { A };", "TZ1622"),
    (in_class("a;"), "TZ1329"),
    (in_class("public readonly a;"), "TZ1329"),
    (in_class("readonly f(): void;"), "TZ1526"),
    (in_class("static abstract f(): void;"), "TZ1528"),
    (in_class("abstract a: int;"), "TZ1529"),
    (in_class("async a: int;"), "TZ1232"),
    (in_class("async constructor();"), "TZ1525"),
    (in_class("abstract constructor();"), "TZ1527"),
    (in_class("override constructor();"), "TZ1527"),
    (in_class("constructor(): C;"), "TZ1523"),
    (in_class("get a(): int;"), "TZ1553"),
    (in_class("set a(value: int);"), "TZ1554"),
    (in_class("static { }"), "TZ1515"),
    (in_class("declare a: int;"), "TZ1516"),
    (in_class("#a: int;"), "TZ1518"),
    (in_class("[key: string]: int;"), "TZ1323"),
    (in_class("1: int;"), "TZ1324"),
    (in_class("*g(): Generator<int>;"), "TZ1325"),
    (in_class("public public a: int;"), "TZ1550"),
    (in_class("public private a: int;"), "TZ1551"),
    (in_class("a: int b: int;"), "TZ1119"),
    (in_class("f(): void g(): void;"), "TZ1119"),
]


def run(exe, mode, path, work=None):
    return subprocess.run([exe, mode, path], capture_output=True, text=True,
                          check=False, encoding="utf-8", cwd=work,
                          timeout=LIMIT_SECONDS)


def write_declaration(work, name, text):
    """NAME.d.tz with the text and an empty module NAME.tz next to it."""
    with open(os.path.join(work, name + ".tz"), "w", newline="\n") as out:
        out.write("")
    path = os.path.join(work, name + ".d.tz")
    with open(path, "w", newline="\n", encoding="utf-8") as out:
        out.write(text)
    return path


POSITION = re.compile(r"^(\s*[A-Za-z]+) -?\d+")
DROPPED = re.compile(r" (?:src|pattern|locs)=\S+")
IMPORT = re.compile(r'^Import \d+ ("[^"]*") \d+ ("[^"]*") -?\d+ (\d) (\d) (\d+)$')
EXPORT = re.compile(r'^Export \d+ (\d) ("[^"]*") -?\d+ (\d+)$')
MODULE_NAME = re.compile(r'^  Name ("[^"]*") \d+ ("[^"]*") \d+$')
SECTION = re.compile(r"^(items|classes|imports|exports) \d+$")
PLACE = re.compile(r"^\s*Declared(?:Class)? (-?\d+) ")


def plain(line):
    for pattern in (IMPORT, EXPORT, MODULE_NAME):
        matched = pattern.match(line)
        if matched:
            word = line.split(" ")[0] or "  Name"
            return " ".join([word] + list(matched.groups()))
    return DROPPED.sub("", POSITION.sub(r"\1", line, count=1))


def sections_of(dump):
    """section -> list of trees, a tree is the list of its raw lines"""
    found = {"errors": []}
    current = None
    for line in dump.split("\n")[1:]:
        if line.startswith("!! "):
            found["errors"].append(line)
        elif SECTION.match(line):
            current = found.setdefault(line.split(" ")[0], [])
        elif line and not line.startswith(" "):
            current.append([line])
        elif line:
            current[-1].append(line)
    return found


SECTION_OF = {"item": "items", "class": "classes", "import": "imports",
              "export": "exports"}


def check_correct(exe, work, cases, report):
    failed = 0
    step = 400
    for start in range(0, len(cases), step):
        part = cases[start:start + step]
        offsets = []
        text = ""
        for case in part:
            offsets.append(len(text))
            text += case.text + "\n\n"
        write_declaration(work, "correct", text)
        dump = run(exe, "tree", "correct.d.tz", work).stdout
        found = sections_of(dump)
        taken = collections.Counter()
        problems = []
        if found["errors"]:
            problems.append("errors: %s" % " ".join(found["errors"][:5]))
        for case, offset in zip(part, offsets):
            section = found.get(SECTION_OF[case.sort], [])
            at = taken[case.sort]
            taken[case.sort] += 1
            raw = section[at] if at < len(section) else []
            actual = [plain(line) for line in raw]
            if actual != case.expected:
                problems.append("tree of:\n%s\n  expected:\n%s\n  actual:\n%s"
                                % (case.text, "\n".join(case.expected),
                                   "\n".join(actual)))
                continue
            places = [int(PLACE.match(line).group(1)) for line in raw
                      if PLACE.match(line)]
            wanted = [offset + name for name in case.names]
            if case.sort == "class" and places != wanted:
                problems.append("places of:\n%s\n  expected %s, actual %s"
                                % (case.text, wanted, places))
        for sort, section in SECTION_OF.items():
            if taken[sort] != len(found.get(section, [])):
                problems.append("%s: %d written, %d in the tree" % (
                    section, taken[sort], len(found.get(section, []))))
        failed += len(problems)
        for problem in problems:
            report.write(problem + "\n\n")
    return failed


ERROR = re.compile(r"^m(\d+)\.d\.tz:\d+:\d+: error (TZ\d+): ", re.M)


def check_mistakes(exe, work, report):
    names = []
    for at, (text, _) in enumerate(MISTAKES):
        name = "m%d" % at
        write_declaration(work, name, "%s\n\n%s\n\n%s\n" % (BEFORE, text,
                                                           AFTER))
        names.append(name + ".d.tz")
    with open(os.path.join(work, "mistakes.list"), "w", newline="\n") as out:
        out.write("\n".join(names) + "\n")
    stderr = run(exe, "check", "@mistakes.list", work).stderr
    codes = collections.defaultdict(list)
    for matched in ERROR.finditer(stderr):
        codes[int(matched.group(1))].append(matched.group(2))
    failed = 0
    for at, (text, code) in enumerate(MISTAKES):
        if codes[at] != [code]:
            failed += 1
            report.write("mistake:\n%s\n  expected %s, actual %s\n\n" % (
                text, code, " ".join(codes[at]) or "no error"))
    for name in names:
        os.remove(os.path.join(work, name))
        os.remove(os.path.join(work, name[:-len(".d.tz")] + ".tz"))
    return failed


def one_line(case):
    return " ".join(part.strip() for part in case.text.split("\n"))


def spoil_bases(cases):
    """One-line items: every construct once, not every modifier with every
    name."""
    seen = set()
    for case in cases:
        line = one_line(case)
        key = re.sub(r"\b(public|protected|private)\b", "public", line)
        key = re.sub(
            r"\b(static|readonly|abstract|override|async) (?=[\w\"\[])", "",
            key)
        if key not in seen or case.sort != "class":
            seen.add(key)
            yield line


def tokens_of(exe, work, lines):
    write_declaration(work, "bases", "\n".join(lines) + "\n")
    offsets = []
    at = 0
    for line in lines:
        offsets.append(at)
        at += len(line) + 1
    found = [[] for _ in lines]
    row = 0
    for record in run(exe, "tokens", "bases.d.tz", work).stdout.split("\n")[1:]:
        parts = record.split(" ")
        if len(parts) != 5 or parts[0] == Mu.END_KIND:
            continue
        start, length = int(parts[1]), int(parts[2])
        while row + 1 < len(offsets) and offsets[row + 1] <= start:
            row += 1
        found[row].append((start - offsets[row],
                           start - offsets[row] + length))
    wrong = run(exe, "each", "bases.d.tz", work).stderr
    assert "error TZ" not in wrong, "a base line is not correct:\n" + wrong[:600]
    return found


def answers(exe, work, lines):
    write_declaration(work, "spoiled", "\n".join(lines) + "\n")
    try:
        done = run(exe, "each", "spoiled.d.tz", work)
        failed = done.returncode not in (0, 1)
        stderr = done.stderr
    except subprocess.TimeoutExpired:
        failed = True
        stderr = ""
    if failed:
        if len(lines) == 1:
            return [None]
        half = len(lines) // 2
        return (answers(exe, work, lines[:half])
                + answers(exe, work, lines[half:]))
    found = [[] for _ in lines]
    for record in stderr.split("\n"):
        matched = Mu.ERROR_LINE.search(record)
        if matched:
            found[int(matched.group(1)) - 1].append(
                (int(matched.group(2)), matched.group(3)))
    return found


def check_spoiled(exe, work, cases, report):
    bases = list(dict.fromkeys(spoil_bases(cases)))
    spans = tokens_of(exe, work, bases)
    spoiled = []
    seen = set()
    for base, (line, tokens) in enumerate(zip(bases, spans)):
        for made in Mu.spoil(base, line, tokens):
            if made.line not in seen:
                seen.add(made.line)
                spoiled.append(made)
    found = answers(exe, work, [made.line for made in spoiled])
    failures = [made for made, got in zip(spoiled, found) if got is None]
    counts = collections.defaultdict(collections.Counter)
    groups = collections.defaultdict(list)
    for made, got in zip(spoiled, found):
        if got is None:
            continue
        sort = "bracket" if Mu.touches_bracket(made) else "plain"
        counts[sort][min(len(got), 3)] += 1
        if sort == "plain" and len(got) > 1:
            codes = " ".join(code for _, code in got)
            groups[(made.kind, made.other or made.token, codes)].append(made)
    report.write("%d one-line items, %d spoiled ones\n" % (len(bases),
                                                           len(spoiled)))
    report.write("the parser failed or hung on %d lines\n" % len(failures))
    for made in failures[:50]:
        report.write("    FAILED %s\n" % made.line)
    report.write("\nerrors per line:        none      one      two    three+\n")
    for sort in sorted(counts):
        row = counts[sort]
        report.write("%-20s %8d %8d %8d %8d\n" % (sort, row[0], row[1],
                                                  row[2], row[3]))
    report.write("\n--- plain tokens, more than one error\n")
    ordered = sorted(groups.items(), key=lambda pair: -len(pair[1]))
    for (kind, what, codes), members in ordered[:80]:
        report.write("=== %d x %s '%s': %s\n" % (len(members), kind, what,
                                                 codes))
        for made in members[:4]:
            report.write("    %-50s <- %s\n" % (made.line, bases[made.base]))
    print("spoiled: %d lines from %d items, failed or hung: %d" % (
        len(spoiled), len(bases), len(failures)))
    for sort in ("plain", "bracket"):
        row = counts[sort]
        print("  %-8s none %d, one %d, two %d, three+ %d" % (
            sort, row[0], row[1], row[2], row[3]))
    return len(failures)


def main():
    exe, work = os.path.abspath(sys.argv[1]), sys.argv[2]
    os.makedirs(work, exist_ok=True)
    cases = list(correct_cases())
    path = os.path.join(work, "declarations.txt")
    with open(path, "w", encoding="utf-8") as report:
        wrong_trees = check_correct(exe, work, cases, report)
        print("correct: %d items, wrong %d" % (len(cases), wrong_trees))
        wrong_mistakes = check_mistakes(exe, work, report)
        print("mistakes: %d, not one error with the code: %d" % (
            len(MISTAKES), wrong_mistakes))
        failed_lines = check_spoiled(exe, work, cases, report)
    print("report: " + path)
    if wrong_trees or wrong_mistakes or failed_lines:
        sys.exit(1)


if __name__ == "__main__":
    main()
