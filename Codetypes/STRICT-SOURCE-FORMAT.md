# Cheat TXT source format — v3.16c

The runtime Cheat Engine parser now requires `$` on every code line: RAW pairs,
RAW continuation/payload rows, F0/F1/F2 headers and bodies, assembly labels, DD
static data and DEADCODE. Leading spaces/tabs before `$` are allowed. Label
definitions still require their colon (`$Done:`); references do not (`$jne Done`).
No automatic dollar or colon insertion is performed by the runtime loader.

```text
!Example Folder:
+Example Code
%Credits: Author
$20000120 00000001 // inline RAW comment
$F0000000 00001000
$pushfd
$jne Done
$Done:
$popfd
$nop
$DEADCODE
$Data:
$dd 12345678
!!
```

Addresses and instructions above are format illustrations, not a usable game patch.
The numeric/byte-order semantics of RAW and F types have not changed.

## Comments and file controls

Blank lines and whole-line `;`, `//` and `#` comments remain ignored. RAW pairs
retain their existing inline `//` and `#` support. Inside F source/data, inline
`;`, `//` and `#` comments remain supported. A trailing semicolon on a RAW pair
is still not accepted; use `//` for inline comments on RAW pairs.

`^` header lines, `!Folder:` opens, `!!` closes, `+Code` / `+OFF`, `%Credits:`,
`{Description}` and the existing PREENTRY controls remain file structure, not
assembly. A colon in a heading never turns that heading into an F label.
This also applies to F-body recovery when DEADCODE is missing.

After F0/F2 `$DEADCODE`, only dollar-prefixed valid label/DD declarations may
extend that hook's data section; the first other meaningful line is left for
the outer parser. F1 still ends with `$DEADCODE 000000NN` (NN=01..08).
The Debugger Code Cave editor enforces the same code-line prefix requirement.

## Rejected definitions

A missing-dollar or malformed source line reports its source line number. The
whole affected cheat definition is prevented from executing — no valid subset
of its RAW writes and no F hook is applied. The rejected definition remains
visible and can be edited or deleted. Other definitions remain independent.
A failed PREENTRY definition is not marked applied. Fix the source and reload
before selecting it again. Structural group warnings remain diagnostics.

For old text files, change `20000120 00000001` to `$20000120 00000001` and add
`$` to bare F instructions, labels, data, terminators and RAW payload rows.
Do not prefix headings or comments. Existing dollar-prefixed files do not
need a rewrite. The older PDF's numeric diagrams describe the code encodings;
this note supersedes any older statement that the dollar prefix is optional.

## Boundaries and RAW payloads

RAW types 3/4/5/6/A keep their existing continuation counts. A prefixed payload
pair beginning with F0/F1/F2 is still literal data, not a nested hook header.
Missing continuation data invalidates that definition instead of absorbing
another code/folder as payload. The runtime loader does not modify the TXT.
