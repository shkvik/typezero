# Языки: `Intl`

Решения, принятые 24 октября 2026 года. Пока ничего из этого не реализовано: порядок работ — в
конце, в разделе «План реализации».

## Коротко

- `Intl` и всё, что пишет или сравнивает по правилам языка, — как в node.
- Данные языков (CLDR) — по тому же правилу, что для дат ([date.md](date.md), раздел 3):
  язык назван в коде строкой — встраивается только он; язык известен только при работе — все
  языки; в Windows — ICU самой системы.

## 1. Что сюда входит

```ts
(1234567.891).toLocaleString("ru-RU")                       // "1 234 567,891"
new Intl.NumberFormat("ru-RU", { style: "currency", currency: "RUB" }).format(1500)
                                                            // "1 500,00 ₽"
["ёж", "яблоко", "арбуз"].sort((a, b) => a.localeCompare(b, "ru"))
                                                            // [ 'арбуз', 'ёж', 'яблоко' ]
new Intl.PluralRules("ru-RU").select(5)                     // "many" — «5 яблок»
"é".normalize("NFD").length                                 // 2
```

- Числа: `toLocaleString`, `Intl.NumberFormat` (валюты, проценты, единицы, компактная запись).
- Сравнение и сортировка строк: `localeCompare`, `Intl.Collator`.
- `Intl.PluralRules`, `Intl.RelativeTimeFormat` («3 дня назад»), `Intl.ListFormat`
  («a, b и c»), `Intl.DisplayNames`, `Intl.Segmenter` (слова и буквы с эмодзи).
- Даты — `Intl.DateTimeFormat`, `toLocaleDateString` — в [date.md](date.md).
- `normalize("NFC" | "NFD" | "NFKC" | "NFKD")` — от языка не зависит; таблицы Unicode
  встраиваются, только если программа вызывает `normalize`.

## 2. Какие данные попадают в exe

| Язык | Что попадает в exe |
|---|---|
| назван в коде строкой: `"ru-RU"`, `"ru"` | только он |
| из переменной | все языки — мегабайты |
| без аргумента — язык системы (`localeCompare(b)`, `toLocaleString()`) | Linux: все языки |
| любой, в Windows | ничего: ICU самой системы |

Встраиваются только нужные части: программе с одним `NumberFormat("ru-RU")` не нужны правила
сортировки. В контейнере язык системы обычно не задан — и node, и tz тогда работают как
`"en-US"`.

## План реализации

На каждом шаге: тесты `tests/run` с `// same-as-node`, обновление README.

1. **Сборка данных CLDR** по частям (числа, сортировка, множественное число, …) и по языкам —
   общая с датами ([date.md](date.md), шаг 4); ICU системы в Windows.
2. **Числа** — `toLocaleString`, `Intl.NumberFormat`.
3. **Строки** — `localeCompare`, `Intl.Collator`, `normalize`, `Intl.Segmenter`.
4. **Остальное** — `PluralRules`, `RelativeTimeFormat`, `ListFormat`, `DisplayNames`.
