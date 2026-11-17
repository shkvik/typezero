"""Spoils one token of a correct program in every possible way and looks at
what the self/ parser answers.

    python mutgen.py <tzparse.exe> <work dir> [more]

The correct programs are the lines the four enumerations build (exprgen,
stmtgen, typegen, declgen): every construct alone and with plain parts;
with 'more', a sample of their pairs too.  One token of a line is deleted,
written twice, swapped with the next one, replaced by another token or gets
another token before it.  Every spoiled line is parsed as a file of its own
('tzparse each').

There is no tree to expect, so the checks are about the answer as a whole:
  - the parser neither stops with a failure nor hangs (when it does, the
    script exits with 1);
  - one spoiled token should give one error.  Lines with two and more are
    counted and grouped; when the spoiled token is a bracket, a quote or a
    part of a template this is no defect, the rest of the line is taken.
"""

import collections
import os
import re
import subprocess
import sys

import declgen as De
import exprgen as E
import stmtgen as St
import typegen as Ty

ERROR_LINE = re.compile(r":(\d+):1:(\d+): error (TZ\d+): ")
END_KIND = "0"

OTHER_TOKENS = [
    ";", ",", "(", ")", "{", "}", "[", "]", ":", "=", "=>", ".", "?", "+",
    "!", "<", ">", "a", "1", '"s"', "if", "else", "class", "function",
    "const", "return", "new", "await", "as", "in", "of",
]
BRACKETS = set("()[]{}<>") | {'"', "'", "`"}
LIMIT_SECONDS = 600


def sampled(items, step):
    for at, item in enumerate(items):
        if at % step == 0:
            yield item


def correct_lines(more):
    found = []
    for item in E.enumerate_levels(["A", "R1"]):
        found.append(E.statement(item.node))
    for item in St.enumerate_levels(["A", "R1"]):
        found.append(item.node.line)
    for item in Ty.enumerate_levels(["A", "R1"]):
        found.append(item.node.line)
    for item in De.enumerate_levels(["A", "R1", "X"]):
        found.append(item.node.line)
    if more:
        for item in sampled(E.enumerate_levels(["L2"]), 25):
            found.append(E.statement(item.node))
        for item in sampled(St.enumerate_levels(["S2"]), 3):
            found.append(item.node.line)
        for item in sampled(Ty.enumerate_levels(["L2", "H"]), 40):
            found.append(item.node.line)
        for item in sampled(De.enumerate_levels(["P2", "T1"]), 12):
            found.append(item.node.line)
    seen = set()
    unique = []
    for line in found:
        line = line.rstrip("\n")
        if line not in seen:
            seen.add(line)
            unique.append(line)
    return unique


def run(exe, mode, path):
    return subprocess.run([exe, mode, path], capture_output=True, text=True,
                          check=False, encoding="utf-8",
                          timeout=LIMIT_SECONDS)


def write_lines(path, lines):
    with open(path, "w", newline="\n", encoding="utf-8") as out:
        for line in lines:
            out.write(line + "\n")


def tokens_of(exe, work, lines):
    """Per line: [(start, end), ...] of its tokens, in the line."""
    path = os.path.join(work, "correct.tz")
    write_lines(path, lines)
    offsets = []
    at = 0
    for line in lines:
        offsets.append(at)
        at += len(line) + 1
    found = [[] for _ in lines]
    row = 0
    for record in run(exe, "tokens", path).stdout.split("\n")[1:]:
        parts = record.split(" ")
        if len(parts) != 5 or parts[0] == END_KIND:
            continue
        start, length = int(parts[1]), int(parts[2])
        while row + 1 < len(offsets) and offsets[row + 1] <= start:
            row += 1
        found[row].append((start - offsets[row], start - offsets[row] + length))
    wrong = run(exe, "each", path).stderr
    assert "error TZ" not in wrong, "a base line is not correct:\n" + wrong[:600]
    os.remove(path)
    return found


Spoiled = collections.namedtuple("Spoiled", "line kind token other at base")


def spoil(base, line, spans):
    """Every way to spoil one token of the line."""
    for at, (start, end) in enumerate(spans):
        token = line[start:end]
        yield Spoiled(line[:start] + " " + line[end:], "delete", token, "",
                      start, base)
        yield Spoiled(line[:end] + " " + token + line[end:], "twice", token,
                      "", end + 1, base)
        if at + 1 < len(spans):
            next_start, next_end = spans[at + 1]
            following = line[next_start:next_end]
            if following != token:
                swapped = (line[:start] + following + line[end:next_start]
                           + token + line[next_end:])
                yield Spoiled(swapped, "swap", token, following, start, base)
        for other in OTHER_TOKENS:
            if other == token:
                continue
            yield Spoiled(line[:start] + other + line[end:], "replace", token,
                          other, start, base)
            yield Spoiled(line[:start] + other + " " + line[start:], "insert",
                          token, other, start, base)


def answers(exe, work, lines):
    """Per line: [(column, code), ...]; None for the lines of a part the
    parser did not finish."""
    path = os.path.join(work, "spoiled.tz")
    write_lines(path, lines)
    try:
        done = run(exe, "each", path)
        failed = done.returncode not in (0, 1)
        stderr = done.stderr
    except subprocess.TimeoutExpired:
        failed = True
        stderr = ""
    os.remove(path)
    if failed:
        if len(lines) == 1:
            return [None]
        half = len(lines) // 2
        return (answers(exe, work, lines[:half])
                + answers(exe, work, lines[half:]))
    found = [[] for _ in lines]
    for record in stderr.split("\n"):
        matched = ERROR_LINE.search(record)
        if matched:
            found[int(matched.group(1)) - 1].append(
                (int(matched.group(2)), matched.group(3)))
    return found


def touches_bracket(spoiled):
    marks = set(spoiled.token) | set(spoiled.other)
    return bool(marks & BRACKETS) or "${" in spoiled.token


def main():
    exe, work = sys.argv[1], sys.argv[2]
    more = len(sys.argv) > 3 and sys.argv[3] == "more"
    os.makedirs(work, exist_ok=True)
    bases = correct_lines(more)
    spans = tokens_of(exe, work, bases)
    spoiled = []
    seen = set()
    for base, (line, tokens) in enumerate(zip(bases, spans)):
        for made in spoil(base, line, tokens):
            if made.line not in seen:
                seen.add(made.line)
                spoiled.append(made)
    found = []
    step = 100000
    for start in range(0, len(spoiled), step):
        part = spoiled[start:start + step]
        found.extend(answers(exe, work, [made.line for made in part]))
        print("  %d of %d" % (min(start + step, len(spoiled)), len(spoiled)),
              flush=True)

    failures = [made for made, got in zip(spoiled, found) if got is None]
    counts = collections.defaultdict(collections.Counter)
    groups = collections.defaultdict(list)
    early = []
    for made, got in zip(spoiled, found):
        if got is None:
            continue
        sort = "bracket" if touches_bracket(made) else "plain"
        size = min(len(got), 3)
        counts[sort][size] += 1
        counts[sort + " " + made.kind][size] += 1
        if sort == "plain" and len(got) > 1:
            codes = " ".join(code for _, code in got)
            what = made.other or made.token
            groups[(made.kind, what, codes)].append(made)
        if got and got[0][0] < made.at + 1 and sort == "plain":
            early.append((made, got))

    report = os.path.join(work, "spoiled.txt")
    with open(report, "w", encoding="utf-8") as out:
        out.write("%d correct lines, %d spoiled ones\n" % (len(bases),
                                                          len(spoiled)))
        out.write("the parser failed or hung on %d lines\n" % len(failures))
        for made in failures[:50]:
            out.write("    FAILED %s\n" % made.line)
        out.write("\nerrors per line:        none      one      two    three+\n")
        for sort in sorted(counts):
            row = counts[sort]
            out.write("%-20s %8d %8d %8d %8d\n" % (sort, row[0], row[1],
                                                    row[2], row[3]))
        out.write("\n--- plain tokens, more than one error: by what was done\n")
        ordered = sorted(groups.items(), key=lambda pair: -len(pair[1]))
        for (kind, what, codes), members in ordered[:80]:
            out.write("=== %d x %s '%s': %s\n" % (len(members), kind, what,
                                                 codes))
            for made in members[:5]:
                out.write("    %-40s <- %s\n" % (made.line, bases[made.base]))
        out.write("\n--- the first error stands before the spoiled token: %d\n"
                  % len(early))
        places = collections.defaultdict(list)
        for made, got in early:
            column, code = got[0]
            word = re.match(r"\w+|\S", made.line[column - 1:] + " ").group()
            places[(code, word)].append((made, column))
        for (code, word), members in sorted(places.items(),
                                            key=lambda pair: -len(pair[1])):
            out.write("=== %d x %s at '%s'\n" % (len(members), code, word))
            for made, column in members[:4]:
                out.write("    col %d, spoiled at %d: %s\n" % (
                    column, made.at + 1, made.line))
        out.write("\n--- every plain line with more than one error\n")
        for (kind, what, codes), members in ordered:
            for made in members:
                out.write("%s\t%s\t%s\t%s\t%s\n" % (kind, what, codes,
                                                    made.line,
                                                    bases[made.base]))
    print("%d correct lines, %d spoiled, failed or hung: %d" % (
        len(bases), len(spoiled), len(failures)))
    for sort in ("plain", "bracket"):
        row = counts[sort]
        print("%-8s none %d, one %d, two %d, three+ %d" % (
            sort, row[0], row[1], row[2], row[3]))
    for made in failures[:20]:
        print("  FAILED " + made.line)
    print("report: " + report)
    if failures:
        sys.exit(1)


if __name__ == "__main__":
    main()
