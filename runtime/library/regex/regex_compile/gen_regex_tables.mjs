const MAX = 0x10ffff;
const isSurrogate = (cp) => cp >= 0xd800 && cp <= 0xdfff;
const hex = (n) => '0x' + n.toString(16).toUpperCase();

const low = [], high = [];
for (let cp = 0; cp < 0xd800; cp++) low.push(String.fromCodePoint(cp));
for (let cp = 0xe000; cp <= MAX; cp++) high.push(String.fromCodePoint(cp));
const lowText = low.join(''), highText = high.join('');

const lastCodePoint = (s) => {
  const end = s.charCodeAt(s.length - 1);
  return end >= 0xdc00 && end <= 0xdfff && s.length > 1 ? s.codePointAt(s.length - 2) : end;
};

function ranges(source) {
  const result = [];
  const run = new RegExp(source + '+', 'gu');
  for (const text of [lowText, highText])
    for (const m of text.matchAll(run)) result.push([m[0].codePointAt(0), lastCodePoint(m[0])]);
  const single = new RegExp('^' + source + '$', 'u');
  for (let cp = 0xd800; cp <= 0xdfff; cp++) {
    if (!single.test(String.fromCharCode(cp))) continue;
    const last = result.find((r) => r[1] === cp - 1);
    if (last) last[1] = cp;
    else result.push([cp, cp]);
  }
  result.sort((a, b) => a[0] - b[0]);
  const merged = [];
  for (const r of result) {
    const last = merged[merged.length - 1];
    if (last && last[1] + 1 >= r[0]) last[1] = Math.max(last[1], r[1]);
    else merged.push([...r]);
  }
  return merged;
}

const accepts = (property) => {
  try {
    new RegExp(`\\p{${property}}`, 'u');
    return true;
  } catch {
    return false;
  }
};

function unicodeClasses() {
  const changes = ranges('[\\p{CWCF}\\p{CWCM}]');
  const classes = new Map();
  const all = lowText + highText;
  for (const [a, b] of changes) {
    for (let cp = a; cp <= b; cp++) {
      if (classes.has(cp)) continue;
      const re = new RegExp(`\\u{${cp.toString(16)}}`, 'giu');
      const members = [...new Set([...all.matchAll(re)].map((m) => m[0].codePointAt(0)))].sort(
        (x, y) => x - y);
      if (members.length < 2) continue;
      for (const m of members) classes.set(m, members);
    }
  }
  return classes;
}

function legacyClasses() {
  const canon = (c) => {
    const u = String.fromCharCode(c).toUpperCase();
    if (u.length !== 1) return c;
    const v = u.charCodeAt(0);
    return c >= 128 && v < 128 ? c : v;
  };
  const byCanon = new Map();
  for (let c = 0; c <= 0xffff; c++) {
    const k = canon(c);
    if (!byCanon.has(k)) byCanon.set(k, []);
    byCanon.get(k).push(c);
  }
  const classes = new Map();
  for (const members of byCanon.values()) {
    if (members.length < 2) continue;
    for (const m of members) classes.set(m, members);
  }
  const all = [];
  for (let c = 0; c <= 0xffff; c++) all.push(String.fromCharCode(c));
  const text = all.join('');
  for (const [c, members] of classes) {
    if (members[0] !== c) continue;
    const re = new RegExp(`\\u${c.toString(16).padStart(4, '0')}`, 'gi');
    const found = [...text.matchAll(re)].map((m) => m.index);
    if (found.join() !== members.join())
      throw new Error(`/i of ${hex(c)}: canonicalize gives ${members}, V8 matches ${found}`);
  }
  return classes;
}

function orbits(classes) {
  const result = [];
  for (const [cp, members] of classes) {
    const i = members.indexOf(cp);
    result.push([cp, members[(i + 1) % members.length]]);
  }
  return result.sort((a, b) => a[0] - b[0]);
}

const leafCategories = [
  ['Lu', 'Uppercase_Letter'], ['Ll', 'Lowercase_Letter'], ['Lt', 'Titlecase_Letter'],
  ['Lm', 'Modifier_Letter'], ['Lo', 'Other_Letter'], ['Mn', 'Nonspacing_Mark'],
  ['Mc', 'Spacing_Mark'], ['Me', 'Enclosing_Mark'], ['Nd', 'Decimal_Number', 'digit'],
  ['Nl', 'Letter_Number'], ['No', 'Other_Number'], ['Pc', 'Connector_Punctuation'],
  ['Pd', 'Dash_Punctuation'], ['Ps', 'Open_Punctuation'], ['Pe', 'Close_Punctuation'],
  ['Pi', 'Initial_Punctuation'], ['Pf', 'Final_Punctuation'], ['Po', 'Other_Punctuation'],
  ['Sm', 'Math_Symbol'], ['Sc', 'Currency_Symbol'], ['Sk', 'Modifier_Symbol'],
  ['So', 'Other_Symbol'], ['Zs', 'Space_Separator'], ['Zl', 'Line_Separator'],
  ['Zp', 'Paragraph_Separator'], ['Cc', 'Control', 'cntrl'], ['Cf', 'Format'],
  ['Cs', 'Surrogate'], ['Co', 'Private_Use'], ['Cn', 'Unassigned'],
];
const groupCategories = [
  [['L', 'Letter'], ['Lu', 'Ll', 'Lt', 'Lm', 'Lo']],
  [['LC', 'Cased_Letter'], ['Lu', 'Ll', 'Lt']],
  [['M', 'Mark', 'Combining_Mark'], ['Mn', 'Mc', 'Me']],
  [['N', 'Number'], ['Nd', 'Nl', 'No']],
  [['P', 'Punctuation', 'punct'], ['Pc', 'Pd', 'Ps', 'Pe', 'Pi', 'Pf', 'Po']],
  [['S', 'Symbol'], ['Sm', 'Sc', 'Sk', 'So']],
  [['Z', 'Separator'], ['Zs', 'Zl', 'Zp']],
  [['C', 'Other'], ['Cc', 'Cf', 'Cs', 'Co', 'Cn']],
];

const scripts = [
  ['Adlam', 'Adlm'], ['Ahom', 'Ahom'], ['Anatolian_Hieroglyphs', 'Hluw'], ['Arabic', 'Arab'],
  ['Armenian', 'Armn'], ['Avestan', 'Avst'], ['Balinese', 'Bali'], ['Bamum', 'Bamu'],
  ['Bassa_Vah', 'Bass'], ['Batak', 'Batk'], ['Bengali', 'Beng'], ['Beria_Erfe', 'Berf'],
  ['Bhaiksuki', 'Bhks'], ['Bopomofo', 'Bopo'], ['Brahmi', 'Brah'], ['Braille', 'Brai'],
  ['Buginese', 'Bugi'], ['Buhid', 'Buhd'], ['Canadian_Aboriginal', 'Cans'], ['Carian', 'Cari'],
  ['Caucasian_Albanian', 'Aghb'], ['Chakma', 'Cakm'], ['Cham', 'Cham'], ['Cherokee', 'Cher'],
  ['Chisoi', 'Chis'], ['Chorasmian', 'Chrs'], ['Common', 'Zyyy'], ['Coptic', 'Copt', 'Qaac'],
  ['Cuneiform', 'Xsux'], ['Cypriot', 'Cprt'], ['Cypro_Minoan', 'Cpmn'], ['Cyrillic', 'Cyrl'],
  ['Deseret', 'Dsrt'], ['Devanagari', 'Deva'], ['Dives_Akuru', 'Diak'], ['Dogra', 'Dogr'],
  ['Duployan', 'Dupl'], ['Egyptian_Hieroglyphs', 'Egyp'], ['Elbasan', 'Elba'],
  ['Elymaic', 'Elym'], ['Ethiopic', 'Ethi'], ['Garay', 'Gara'], ['Georgian', 'Geor'],
  ['Glagolitic', 'Glag'], ['Gothic', 'Goth'], ['Grantha', 'Gran'], ['Greek', 'Grek'],
  ['Gujarati', 'Gujr'], ['Gunjala_Gondi', 'Gong'], ['Gurmukhi', 'Guru'],
  ['Gurung_Khema', 'Gukh'], ['Han', 'Hani'], ['Hangul', 'Hang'], ['Hanifi_Rohingya', 'Rohg'],
  ['Hanunoo', 'Hano'], ['Hatran', 'Hatr'], ['Hebrew', 'Hebr'], ['Hiragana', 'Hira'],
  ['Imperial_Aramaic', 'Armi'], ['Inherited', 'Zinh', 'Qaai'], ['Inscriptional_Pahlavi', 'Phli'],
  ['Inscriptional_Parthian', 'Prti'], ['Javanese', 'Java'], ['Kaithi', 'Kthi'],
  ['Kannada', 'Knda'], ['Katakana', 'Kana'], ['Kawi', 'Kawi'], ['Kayah_Li', 'Kali'],
  ['Kharoshthi', 'Khar'], ['Khitan_Small_Script', 'Kits'], ['Khmer', 'Khmr'],
  ['Khojki', 'Khoj'], ['Khudawadi', 'Sind'], ['Kirat_Rai', 'Krai'], ['Lao', 'Laoo'],
  ['Latin', 'Latn'], ['Lepcha', 'Lepc'], ['Limbu', 'Limb'], ['Linear_A', 'Lina'],
  ['Linear_B', 'Linb'], ['Lisu', 'Lisu'], ['Lycian', 'Lyci'], ['Lydian', 'Lydi'],
  ['Mahajani', 'Mahj'], ['Makasar', 'Maka'], ['Malayalam', 'Mlym'], ['Mandaic', 'Mand'],
  ['Manichaean', 'Mani'], ['Marchen', 'Marc'], ['Masaram_Gondi', 'Gonm'],
  ['Medefaidrin', 'Medf'], ['Meetei_Mayek', 'Mtei'], ['Mende_Kikakui', 'Mend'],
  ['Meroitic_Cursive', 'Merc'], ['Meroitic_Hieroglyphs', 'Mero'], ['Miao', 'Plrd'],
  ['Modi', 'Modi'], ['Mongolian', 'Mong'], ['Mro', 'Mroo'], ['Multani', 'Mult'],
  ['Myanmar', 'Mymr'], ['Nabataean', 'Nbat'], ['Nag_Mundari', 'Nagm'], ['Nandinagari', 'Nand'],
  ['New_Tai_Lue', 'Talu'], ['Newa', 'Newa'], ['Nko', 'Nkoo'], ['Nushu', 'Nshu'],
  ['Nyiakeng_Puachue_Hmong', 'Hmnp'], ['Ogham', 'Ogam'], ['Ol_Chiki', 'Olck'],
  ['Ol_Onal', 'Onao'], ['Old_Hungarian', 'Hung'], ['Old_Italic', 'Ital'],
  ['Old_North_Arabian', 'Narb'], ['Old_Permic', 'Perm'], ['Old_Persian', 'Xpeo'],
  ['Old_Sogdian', 'Sogo'], ['Old_South_Arabian', 'Sarb'], ['Old_Turkic', 'Orkh'],
  ['Old_Uyghur', 'Ougr'], ['Oriya', 'Orya'], ['Osage', 'Osge'], ['Osmanya', 'Osma'],
  ['Pahawh_Hmong', 'Hmng'], ['Palmyrene', 'Palm'], ['Pau_Cin_Hau', 'Pauc'],
  ['Phags_Pa', 'Phag'], ['Phoenician', 'Phnx'], ['Psalter_Pahlavi', 'Phlp'], ['Rejang', 'Rjng'],
  ['Runic', 'Runr'], ['Samaritan', 'Samr'], ['Saurashtra', 'Saur'], ['Sharada', 'Shrd'],
  ['Shavian', 'Shaw'], ['Siddham', 'Sidd'], ['Sidetic', 'Sidt'], ['SignWriting', 'Sgnw'],
  ['Sinhala', 'Sinh'], ['Sogdian', 'Sogd'], ['Sora_Sompeng', 'Sora'], ['Soyombo', 'Soyo'],
  ['Sundanese', 'Sund'], ['Sunuwar', 'Sunu'], ['Syloti_Nagri', 'Sylo'], ['Syriac', 'Syrc'],
  ['Tagalog', 'Tglg'], ['Tagbanwa', 'Tagb'], ['Tai_Le', 'Tale'], ['Tai_Tham', 'Lana'],
  ['Tai_Viet', 'Tavt'], ['Tai_Yo', 'Tayo'], ['Takri', 'Takr'], ['Tamil', 'Taml'],
  ['Tangsa', 'Tnsa'], ['Tangut', 'Tang'], ['Telugu', 'Telu'], ['Thaana', 'Thaa'],
  ['Thai', 'Thai'], ['Tibetan', 'Tibt'], ['Tifinagh', 'Tfng'], ['Tirhuta', 'Tirh'],
  ['Todhri', 'Todr'], ['Tolong_Siki', 'Tols'], ['Toto', 'Toto'], ['Tulu_Tigalari', 'Tutg'],
  ['Ugaritic', 'Ugar'], ['Unknown', 'Zzzz'], ['Vai', 'Vaii'], ['Vithkuqi', 'Vith'],
  ['Wancho', 'Wcho'], ['Warang_Citi', 'Wara'], ['Yezidi', 'Yezi'], ['Yi', 'Yiii'],
  ['Zanabazar_Square', 'Zanb'],
].filter(([name]) => accepts(`Script=${name}`));

const binaries = [
  ['ASCII'], ['ASCII_Hex_Digit', 'AHex'], ['Alphabetic', 'Alpha'], ['Any'], ['Assigned'],
  ['Bidi_Control', 'Bidi_C'], ['Bidi_Mirrored', 'Bidi_M'], ['Case_Ignorable', 'CI'], ['Cased'],
  ['Changes_When_Casefolded', 'CWCF'], ['Changes_When_Casemapped', 'CWCM'],
  ['Changes_When_Lowercased', 'CWL'], ['Changes_When_NFKC_Casefolded', 'CWKCF'],
  ['Changes_When_Titlecased', 'CWT'], ['Changes_When_Uppercased', 'CWU'], ['Dash'],
  ['Default_Ignorable_Code_Point', 'DI'], ['Deprecated', 'Dep'], ['Diacritic', 'Dia'],
  ['Emoji'], ['Emoji_Component', 'EComp'], ['Emoji_Modifier', 'EMod'],
  ['Emoji_Modifier_Base', 'EBase'], ['Emoji_Presentation', 'EPres'],
  ['Extended_Pictographic', 'ExtPict'], ['Extender', 'Ext'], ['Grapheme_Base', 'Gr_Base'],
  ['Grapheme_Extend', 'Gr_Ext'], ['Hex_Digit', 'Hex'], ['IDS_Binary_Operator', 'IDSB'],
  ['IDS_Trinary_Operator', 'IDST'], ['ID_Continue', 'IDC'], ['ID_Start', 'IDS'],
  ['Ideographic', 'Ideo'], ['Join_Control', 'Join_C'], ['Logical_Order_Exception', 'LOE'],
  ['Lowercase', 'Lower'], ['Math'], ['Noncharacter_Code_Point', 'NChar'],
  ['Pattern_Syntax', 'Pat_Syn'], ['Pattern_White_Space', 'Pat_WS'], ['Quotation_Mark', 'QMark'],
  ['Radical'], ['Regional_Indicator', 'RI'], ['Sentence_Terminal', 'STerm'],
  ['Soft_Dotted', 'SD'], ['Terminal_Punctuation', 'Term'], ['Unified_Ideograph', 'UIdeo'],
  ['Uppercase', 'Upper'], ['Variation_Selector', 'VS'], ['White_Space', 'space'],
  ['XID_Continue', 'XIDC'], ['XID_Start', 'XIDS'],
].filter(([name]) => accepts(name));

const out = [];
const emitList = (type, name, items, perLine, format) => {
  out.push(`static const ${type} ${name}[] = {`);
  for (let i = 0; i < items.length; i += perLine)
    out.push('    ' + items.slice(i, i + perLine).map(format).join(' '));
  out.push('};');
};

emitList('ReOrbit', 're_orbits_unicode', orbits(unicodeClasses()), 4,
         ([a, b]) => `{${hex(a)}, ${hex(b)}},`);
emitList('ReOrbit', 're_orbits_legacy', orbits(legacyClasses()), 4,
         ([a, b]) => `{${hex(a)}, ${hex(b)}},`);
out.push('');

const rangeData = [];
const addRanges = (list) => {
  const offset = rangeData.length;
  for (const r of list) rangeData.push(r);
  return [offset, list.length];
};

const category = new Uint8Array(MAX + 1);
leafCategories.forEach(([short], index) => {
  for (const [a, b] of ranges(`\\p{gc=${short}}`)) category.fill(index, a, b + 1);
});
const gcStarts = [];
for (let cp = 0; cp <= MAX; cp++)
  if (cp === 0 || category[cp] !== category[cp - 1]) gcStarts.push([cp, category[cp]]);
emitList('ReRun', 're_categories', gcStarts, 6, ([cp, v]) => `{${hex(cp)}, ${v}},`);
out.push('');

const script = new Uint16Array(MAX + 1);
scripts.forEach(([name], index) => {
  for (const [a, b] of ranges(`\\p{Script=${name}}`)) script.fill(index, a, b + 1);
});
const scStarts = [];
for (let cp = 0; cp <= MAX; cp++)
  if (cp === 0 || script[cp] !== script[cp - 1]) scStarts.push([cp, script[cp]]);
emitList('ReRun', 're_scripts', scStarts, 6, ([cp, v]) => `{${hex(cp)}, ${v}},`);
out.push('');

const scxLists = scripts.map(([name]) => addRanges(ranges(`\\p{Script_Extensions=${name}}`)));
const binaryLists = binaries.map(([name]) => addRanges(ranges(`\\p{${name}}`)));
emitList('ReRange', 're_ranges', rangeData, 4, ([a, b]) => `{${hex(a)}, ${hex(b)}},`);
emitList('ReList', 're_script_extensions', scxLists, 6, ([o, n]) => `{${o}, ${n}},`);
emitList('ReList', 're_binary_properties', binaryLists, 6, ([o, n]) => `{${o}, ${n}},`);
out.push('');

const names = [];
leafCategories.forEach((aliases, index) => {
  for (const a of aliases) names.push([a, 'RE_NAME_CATEGORY', 2 ** index]);
});
for (const [aliases, leaves] of groupCategories) {
  const mask = leaves.reduce((m, l) => m + 2 ** leafCategories.findIndex(([s]) => s === l), 0);
  for (const a of aliases) names.push([a, 'RE_NAME_CATEGORY', mask]);
}
scripts.forEach((aliases, index) => {
  for (const a of new Set(aliases)) names.push([a, 'RE_NAME_SCRIPT', index]);
});
binaries.forEach((aliases, index) => {
  for (const a of aliases) names.push([a, 'RE_NAME_BINARY', index]);
});
names.sort((a, b) => (a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : a[1] < b[1] ? -1 : 1));
for (const [name, kind, value] of names)
  if (!accepts(kind === 'RE_NAME_SCRIPT' ? `sc=${name}` : name))
    throw new Error(`Node rejects the property name ${name}`);
out.push('static const ReName re_property_names[] = {');
for (const [name, kind, value] of names) out.push(`    {"${name}", ${kind}, ${hex(value)}},`);
out.push('};');
out.push(`#define RE_BINARY_ANY ${binaries.findIndex(([n]) => n === 'Any')}`);
out.push(`#define RE_BINARY_ID_START ${binaries.findIndex(([n]) => n === 'ID_Start')}`);
out.push(`#define RE_BINARY_ID_CONTINUE ${binaries.findIndex(([n]) => n === 'ID_Continue')}`);
out.push('');

process.stdout.write(out.join('\n'));
