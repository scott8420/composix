# Hyphenation patterns (s038; more languages s190)

The TeX hyphenation patterns (and exceptions, where a language has them) as
published in the hyph-utf8 package (github.com/hyphenation/tex-hyphen,
`hyph-utf8/tex/generic/hyph-utf8/patterns/txt/`), unmodified:

| File | Language | Minimums (left, right) | Licence |
|---|---|---|---|
| `hyph-en-us.*` | English (US) -- ushyphmax, Gerard D.C. Kuiken; Knuth's hyphen.tex | 2, 3 | copying permitted, keeping the notice |
| `hyph-en-gb.*` | English (UK) -- Dominik Wujastyk, Graham Toal | 2, 3 | MIT |
| `hyph-de-1996.pat.txt` | German, reformed spelling -- Deutschsprachige Trennmustermannschaft | 2, 2 | MIT |
| `hyph-fr.pat.txt` | French -- Daniel Flipo, Bernard Gaulle, Arthur Reutenauer | 2, 2 | MIT |
| `hyph-es.pat.txt` | Spanish -- Javier Bezos, CervanTeX | 2, 2 | MIT/X11 |
| `hyph-it.pat.txt` | Italian -- Claudio Beccari | 2, 2 | LPPL or MIT (MIT taken) |
| `hyph-nl.*` | Dutch -- Piet Tutelaers | 2, 2 | MIT |
| `hyph-pt.*` | Portuguese -- Pedro J. de Rezende, J. Joao Dias Almeida, Leonardo Araujo, Aline Benevides | 2, 3 | BSD 3-clause |

Each language's copyright and licence text: `NOTICE-hyph-<lang>.txt` (the
header of its `.tex` file in hyph-utf8).

CMake embeds every file into the build (`HyphenData.cpp`, generated), so
Composix needs no hyphenation package at run time.
