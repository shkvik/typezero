const MAX = 0x10ffff;
const isSurrogate = (cp) => cp >= 0xd800 && cp <= 0xdfff;

function mappings(convert) {
  const simple = [];
  const special = [];
  for (let cp = 0; cp <= MAX; cp++) {
    if (isSurrogate(cp)) continue;
    const to = [...convert(String.fromCodePoint(cp))].map((c) => c.codePointAt(0));
    if (to.length === 1) {
      if (to[0] !== cp) simple.push([cp, to[0] - cp]);
    } else {
      special.push([cp, to]);
    }
  }
  const runs = [];
  for (const [cp, delta] of simple) {
    const run = runs[runs.length - 1];
    if (run && run.delta === delta) {
      const step = cp - run.end;
      if ((run.end === run.start && (step === 1 || step === 2)) || step === run.stride) {
        run.stride = step;
        run.end = cp;
        continue;
      }
    }
    runs.push({ start: cp, end: cp, stride: 1, delta });
  }
  return { runs, special };
}

function ranges(regex) {
  const result = [];
  for (let cp = 0; cp <= MAX; cp++) {
    if (isSurrogate(cp) || !regex.test(String.fromCodePoint(cp))) continue;
    const last = result[result.length - 1];
    if (last && last[1] === cp - 1) last[1] = cp;
    else result.push([cp, cp]);
  }
  return result;
}

const hex = (n) => '0x' + n.toString(16).toUpperCase();
const out = [];
for (const [name, convert] of [['upper', (s) => s.toUpperCase()], ['lower', (s) => s.toLowerCase()]]) {
  const { runs, special } = mappings(convert);
  out.push(`static const CaseRun ${name}_runs[] = {`);
  for (const r of runs) out.push(`    {${hex(r.start)}, ${hex(r.end)}, ${r.stride}, ${r.delta}},`);
  out.push('};');
  out.push(`static const CaseSpecial ${name}_special[] = {`);
  for (const [cp, to] of special) {
    const units = [...to, 0, 0].slice(0, 3);
    out.push(`    {${hex(cp)}, ${to.length}, {${units.map(hex).join(', ')}}},`);
  }
  out.push('};');
  out.push('');
}

for (const [name, regex] of [['cased', /\p{Cased}/u], ['case_ignorable', /\p{Case_Ignorable}/u]]) {
  out.push(`static const CodeRange ${name}_ranges[] = {`);
  for (const [a, b] of ranges(regex)) out.push(`    {${hex(a)}, ${hex(b)}},`);
  out.push('};');
  out.push('');
}

process.stdout.write(out.join('\n'));
