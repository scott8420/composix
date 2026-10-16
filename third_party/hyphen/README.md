# Hyphenation patterns (s038)

`hyph-en-us.pat.txt` / `hyph-en-us.hyp.txt`: the TeX hyphenation patterns
and exceptions for American English (ushyphmax, Gerard D.C. Kuiken; Knuth's
hyphen.tex), as published in the hyph-utf8 package
(github.com/hyphenation/tex-hyphen, `hyph-utf8/tex/generic/hyph-utf8/patterns/txt/`).
Unmodified. Licence and copyright: `NOTICE-hyph-en-us.txt` (copying and
distribution permitted, with or without modification, keeping the notice).

CMake embeds both files into the build (`HyphenData.cpp`, generated), so
Composix needs no hyphenation package at run time.
