"""Puts every mistake the parser knows into many places and checks that it
answers with the error of that mistake and nothing else.

    python wronggen.py <tzparse.exe> <work dir> [code ...]

The mistakes are the sources of errors/TZxxxx.tz here, one per error
code.  A source is used whole as a statement; when it is a declaration of a
variable, of a type or of a class with one member, its value, its type or
its member is used as an expression, a type or a member too.  The places
are fixed lines of correct code around the mistake; every line is parsed as
a file of its own.

The mistakes are of three sorts:
  closed  the error is inside the source, and what comes after it on the
          line is not touched: in every place the answer must be exactly
          the expected error;
  open    the source breaks off (a bracket, a string or a comment that is
          never closed): it takes what follows on the line, so other errors
          after the expected one are counted but are no defect;
  lines   the source needs its line breaks and is tried only as it is.

For every line the answer is one of:
  ok     exactly the expected error;
  noise  the expected error first, then others;
  other  another error first, or none.

The script exits with 1 when a line of a closed mistake is not ok and is not
listed in KNOWN.

The tests of declaration files (TZxxxx.d.tz and the module next to it) are
left out: such a mistake needs a file of its own; dfilegen has them.
"""

import collections
import glob
import os
import re
import subprocess
import sys

ERROR_LINE = re.compile(r"w(\d+)\.tz:\d+:(\d+): error (TZ\d+): ")

# What the lexer never closes takes the rest of the line by its nature.
NEVER_CLOSED = ("TZ1001", "TZ1006", "TZ1007", "TZ1008")

# Lines where the place changes what the mistake is, so another error is the
# right answer: '<' of the type argument and '<' of the mistake make '<<',
# the '>' of 'Array<int>' closes the type parameters of the function, and
# as the body of an 'if' the statement before 'else' gives that 'else' its
# 'if'.
KNOWN = {
    "if (a) run(); else { stop(); };",
    "type T = A<<T>(a: T) T>;",
    "type T = A<<T>(a: T) T, B>;",
    "type T = A<<T>(a: T) => T>;",
    "type T = A<<T>(a: T) => T, B>;",
    "function f<U extends Array<int>() { }",
}

TOP = "top"
NESTED = "nested"

STATEMENT_PLACES = [
    ("alone", TOP, "%s"),
    ("after a statement", TOP, "a; %s"),
    ("before a statement", TOP, "%s b;"),
    ("then a function", TOP, "%s function g() { }"),
    ("then a class", TOP, "%s class B { }"),
    ("between statements", NESTED, "{ a; %s b; }"),
    ("in a block", NESTED, "{ %s }"),
    ("in if {}", NESTED, "if (a) { %s }"),
    ("in if {} else", NESTED, "if (a) { %s } else { b; }"),
    ("in else {}", NESTED, "if (a) { b; } else { %s }"),
    ("in while {}", NESTED, "while (a) { %s }"),
    ("in for {}", NESTED, "for (const v of a) { %s }"),
    ("in a function", NESTED, "function f() { %s }"),
    ("in an arrow function", NESTED, "const g = () => { %s };"),
    ("in a method", NESTED, "class A { m() { %s } }"),
    ("in a case", NESTED, "switch (a) { case b: %s }"),
    ("body of if", NESTED, "if (a) %s"),
    ("body of while", NESTED, "while (a) %s"),
    ("after a label", NESTED, "l: %s"),
]

EXPRESSION_PLACES = [
    ("in parentheses", "(%s);"),
    ("value of const", "const v = %s;"),
    ("argument", "f(%s);"),
    ("second argument", "f(a, %s);"),
    ("first argument", "f(%s, b);"),
    ("element", "const v = [%s];"),
    ("first element", "const v = [%s, b];"),
    ("field", "const v = { k: %s };"),
    ("first field", "const v = { k: %s, m: b };"),
    ("left of +", "const v = (%s) + b;"),
    ("right of +", "const v = a + (%s);"),
    ("condition of ?:", "const v = (%s) ? b : c;"),
    ("then of ?:", "const v = a ? (%s) : c;"),
    ("else of ?:", "const v = a ? b : (%s);"),
    ("condition of if", "if (%s) { b; }"),
    ("condition of while", "while (%s) { b; }"),
    ("value of return", "function f() { return %s; }"),
    ("body of arrow", "const g = (p) => (%s);"),
    ("default of parameter", "function f(p = %s) { }"),
    ("index", "const v = a[%s];"),
    ("in a template", "const v = `p${%s}q`;"),
    ("value of a field of a class", "class A { x = %s; }"),
    ("value of an enum member", "enum E { A = %s }"),
    ("value of case", "switch (a) { case %s: b; }"),
    ("for of", "for (const v of %s) { b; }"),
]

TYPE_PLACES = [
    ("alias", "type T = %s;"),
    ("of let", "let v: %s;"),
    ("of const", "const v: %s = a;"),
    ("of a parameter", "function f(p: %s) { }"),
    ("of a result", "function f(): %s { }"),
    ("of an arrow parameter", "const g = (p: %s) => a;"),
    ("type argument", "type T = A<%s>;"),
    ("second type argument", "type T = A<B, %s>;"),
    ("first type argument", "type T = A<%s, B>;"),
    ("element of an array", "type T = (%s)[];"),
    ("member of a union", "type T = A | (%s);"),
    ("first member of a union", "type T = (%s) | B;"),
    ("field of an object type", "type T = { a: %s };"),
    ("first field of an object type", "type T = { a: %s; b: B };"),
    ("element of a tuple", "type T = [%s];"),
    ("first element of a tuple", "type T = [%s, B];"),
    ("result of a function type", "type T = (p: A) => %s;"),
    ("parameter of a function type", "type T = (p: %s) => B;"),
    ("of a field of a class", "class A { x: %s; }"),
    ("of a field of an interface", "interface I { a: %s }"),
    ("limit of a type parameter", "function f<U extends %s>() { }"),
    ("after as", "const v = a as %s;"),
]

MEMBER_PLACES = [
    ("only member", "class A { %s }"),
    ("after a field", "class A { x: int; %s }"),
    ("before a field", "class A { %s y: int; }"),
    ("after a method", "class A { m() { } %s }"),
    ("before a method", "class A { %s n() { } }"),
    ("between members", "class A { x: int; %s n() { } }"),
    ("in an abstract class", "abstract class A { %s }"),
    ("in an exported class", "export class A { %s }"),
]

VARIABLE = re.compile(r"^(?:const|let) \w+ = (.+?);?$")
ALIAS = re.compile(r"^type \w+(?:<[^=]*>)? = (.+?);?$")
TYPED = re.compile(r"^let \w+: (.+?);?$")
CLASS = re.compile(r"^(?:abstract )?class \w+ \{ (.+) \}$")
MODULE_WORD = re.compile(r"^(?:import|export)\b")


def read_mistakes(root, wanted):
    """Per error code: its source on one line."""
    found = {}
    for path in sorted(glob.glob(os.path.join(root, "TZ*.tz"))):
        code = os.path.basename(path)[:-3]
        if wanted and code not in wanted:
            continue
        if code.endswith(".d") or os.path.exists(path[:-3] + ".d.tz"):
            continue
        with open(path, encoding="utf-8") as source:
            lines = [line.strip() for line in source.read().split("\n")]
        found[code] = " ".join(line for line in lines if line)
    return found


ENDS_WITH_BLOCK = re.compile(
    r"^(?:export |declare |abstract |partial |async |@\w+ )*"
    r"(?:function|class|enum|interface|namespace|global|if|for|while|switch"
    r"|do|try|catch|finally|l:|\{|\})")


def closed(text):
    """The statement with ';' after it unless it ends a statement itself: it
    has one already, or it is a statement that ends with a block."""
    listed = text.startswith(("export {", "import {"))
    ends_itself = (text.endswith("}") and ENDS_WITH_BLOCK.match(text)
                   and not listed)
    if text.endswith(";") or ends_itself:
        return text
    return text + ";"


def lines_for(text):
    """Every line a mistake is tried in: (kind, place, line)."""
    made = []
    top_only = MODULE_WORD.match(text) is not None
    for place, level, pattern in STATEMENT_PLACES:
        if level == TOP or not top_only:
            made.append(("statement", place, pattern % closed(text)))
    value = VARIABLE.match(text)
    if value:
        for place, pattern in EXPRESSION_PLACES:
            made.append(("expression", place, pattern % value.group(1)))
    typed = ALIAS.match(text) or TYPED.match(text)
    if typed:
        for place, pattern in TYPE_PLACES:
            made.append(("type", place, pattern % typed.group(1)))
    member = CLASS.match(text)
    if member:
        for place, pattern in MEMBER_PLACES:
            written = member.group(1)
            if not written.endswith((";", "}")):
                written += ";"
            made.append(("member", place, pattern % written))
    return made


def answers(exe, work, lines):
    """What the parser reports for every line: [(column, code), ...].  Each
    line is a file of its own: an unclosed bracket must not reach the next
    mistake."""
    folder = os.path.join(work, "files")
    os.makedirs(folder, exist_ok=True)
    names = []
    for at, line in enumerate(lines):
        name = "w%06d.tz" % at
        names.append(name)
        with open(os.path.join(folder, name), "w", newline="\n",
                  encoding="utf-8") as out:
            out.write(line + "\n")
    with open(os.path.join(folder, "list.txt"), "w", newline="\n") as out:
        out.write("\n".join(names) + "\n")
    checked = subprocess.run([exe, "check", "@list.txt"], cwd=folder,
                             capture_output=True, text=True, check=False,
                             encoding="utf-8")
    reported = collections.defaultdict(list)
    for line in checked.stderr.split("\n"):
        found = ERROR_LINE.search(line)
        if found:
            reported[int(found.group(1))].append(
                (int(found.group(2)), found.group(3)))
    for name in names + ["list.txt"]:
        os.remove(os.path.join(folder, name))
    os.rmdir(folder)
    return reported


QUOTED = re.compile(r'"(?:[^"\\]|\\.)*"')
PAIRS = {")": "(", "]": "[", "}": "{"}


def balanced(text):
    """Does every bracket of the text have its pair?"""
    open_ones = []
    for mark in QUOTED.sub('""', text):
        if mark in "([{":
            open_ones.append(mark)
        elif mark in PAIRS:
            if not open_ones or open_ones.pop() != PAIRS[mark]:
                return False
    return not open_ones


def sort_of(code, text, alone, alone_closed):
    """closed, open or lines: see the head of the file."""
    if [found for _, found in alone] != [code]:
        return "lines"
    if code in NEVER_CLOSED or not balanced(text):
        return "open"
    at_end = alone[0][0] > len(text)
    still = [found for _, found in alone_closed] == [code]
    return "open" if at_end or not still else "closed"


def verdict(code, reported):
    if reported == [code]:
        return "ok"
    if reported and reported[0] == code:
        return "noise"
    return "other"


def main():
    exe, work = sys.argv[1], sys.argv[2]
    wanted = sys.argv[3:]
    os.makedirs(work, exist_ok=True)
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.join(here, "errors")
    mistakes = read_mistakes(root, wanted)

    listed = list(mistakes.items())
    first = answers(exe, work, [text for _, text in listed]
                    + [closed(text) for _, text in listed])
    sorts = {}
    for at, (code, text) in enumerate(listed):
        sorts[code] = sort_of(code, text, first.get(at, []),
                              first.get(at + len(listed), []))

    tried = []
    for code, text in listed:
        if sorts[code] == "lines":
            continue
        for kind, place, line in lines_for(text):
            tried.append((code, kind, place, line))
    found = answers(exe, work, [row[3] for row in tried])

    # A value, a type or a member taken out of the source counts only when
    # it is the mistake: in its first, simplest place it must give the error.
    first_of_kind = {}
    for at, (code, kind, place, line) in enumerate(tried):
        first_of_kind.setdefault((code, kind), at)
    rows = []
    reported = {}
    for at, row in enumerate(tried):
        base = first_of_kind[(row[0], row[1])]
        codes = [name for _, name in found.get(base, [])]
        if row[1] == "statement" or codes == [row[0]]:
            reported[len(rows)] = found.get(at, [])
            rows.append(row)

    totals = collections.defaultdict(collections.Counter)
    by_place = collections.defaultdict(collections.Counter)
    by_code = collections.defaultdict(collections.Counter)
    by_extra = collections.defaultdict(list)
    not_ok = []
    for at, (code, kind, place, line) in enumerate(rows):
        got = [found for _, found in reported.get(at, [])]
        answer = verdict(code, got)
        sort = sorts[code]
        totals[sort][answer] += 1
        by_place[(sort, kind, place)][answer] += 1
        by_code[(sort, code, kind)][answer] += 1
        if answer == "ok":
            continue
        shown = " ".join(got) or "no error"
        not_ok.append((sort, answer, kind, place, code, shown, line))
        if sort == "closed":
            key = shown if answer == "other" else "+ " + " ".join(got[1:])
            by_extra[(answer, key)].append((code, kind, place, line))

    report = os.path.join(work, "wrong.txt")
    with open(report, "w", encoding="utf-8") as out:
        for sort in ("closed", "open"):
            counts = totals[sort]
            out.write("%-6s %3d mistakes, %5d lines: ok %d, noise %d, other %d\n"
                      % (sort, sum(1 for s in sorts.values() if s == sort),
                         sum(counts.values()), counts["ok"], counts["noise"],
                         counts["other"]))
        alone_only = sorted(code for code, sort in sorts.items()
                            if sort == "lines")
        out.write("lines  %3d mistakes, tried only as they are: %s\n"
                  % (len(alone_only), " ".join(alone_only)))
        out.write("\n--- closed mistakes: what is reported instead\n")
        ordered = sorted(by_extra.items(), key=lambda pair: -len(pair[1]))
        for (answer, key), members in ordered:
            out.write("=== %d x %s: %s\n" % (len(members), answer, key))
            for code, kind, place, line in members[:10]:
                out.write("    %s %-10s %-26s %s\n" % (code, kind, place,
                                                       line))
        out.write("\n--- by place: ok / noise / other\n")
        for (sort, kind, place), counts in sorted(by_place.items()):
            out.write("%-6s %-11s %-30s %4d %4d %4d\n" % (
                sort, kind, place, counts["ok"], counts["noise"],
                counts["other"]))
        out.write("\n--- by code: ok / noise / other\n")
        for (sort, code, kind), counts in sorted(by_code.items()):
            out.write("%-6s %s %-11s %4d %4d %4d   %s\n" % (
                sort, code, kind, counts["ok"], counts["noise"],
                counts["other"], mistakes[code][:60]))
        out.write("\n--- every line that is not ok\n")
        for row in sorted(not_ok):
            out.write("%s\t%s\t%s\t%s\t%s\t%s\t%s\n" % row)
    for sort in ("closed", "open"):
        counts = totals[sort]
        print("%-6s %5d lines: ok %d, noise %d, other %d" % (
            sort, sum(counts.values()), counts["ok"], counts["noise"],
            counts["other"]))
    new = [row for row in sorted(not_ok)
           if row[0] == "closed" and row[6] not in KNOWN]
    for sort, answer, kind, place, code, shown, line in new[:20]:
        print("  %s in '%s' gives %s: %s" % (code, place, shown, line))
    print("report: " + report)
    if new:
        sys.exit(1)


if __name__ == "__main__":
    main()
