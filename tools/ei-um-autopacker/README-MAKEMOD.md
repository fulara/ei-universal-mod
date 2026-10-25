# EI Multitool

## How to use

This is a special tool I've made to automate packing the EI assets, it uses Rsync and GNU Parallel.
This tool can't run on Windows nor be ported to it.

To use it, open a bash terminal in this directy, and run `bash makemod.sh`

## WARNINGS

- Do not delete directories, the script uses many hardcoded paths.
- Resouces must be symlinked instead of being copy pasted, paths must be relative and not absolute.
- `makemod.sh` must be executed from this directory.

## Language packs

Python 3 stages non-English language packs over the current English sources before
calling `bin/um-multitool`. Missing, empty, malformed, stale-control, or
unencodable translations produce warnings and use current English instead.
Stale keys are omitted. Canonical source, tool, and I/O failures stop the build.
English fallback bytes are preserved; only accepted translations are transcoded.

Polish sources use UTF-8 without BOM; packaging converts their payloads to CP1250.
Other languages keep their source bytes unless encoding options are supplied.
`string version_name`, `map-lmp.txt`, and all nested quest metadata always come
from English. Quest translations contain only current top-level `quest ...` and
`briefing ...` files; every `#` directive and objective ID must match English.
The helper never writes generated REG files into the source directories.

The three unfinished `z31q1` briefings are intentionally absent from `mq-pol`:
their English sources contain author placeholders, so builds warn and keep those
English stubs. Existing `<placeholder>` item descriptions are preserved rather
than replaced with invented text.

The helper has two subcommands:

```bash
python3 bin/pack_language.py texts --tool bin/um-multitool \
  --english res-texts/texts-eng_res --translation res-texts/texts-pol_res \
  --output /tmp/texts-pol.res --source-encoding utf-8 --target-encoding cp1250
python3 bin/pack_language.py quests --tool bin/um-multitool \
  --english mq-unpacked/mq-eng --translation mq-unpacked/mq-pol \
  --output /tmp/pol-maps --source-encoding utf-8 --target-encoding cp1250
```

Both require `--tool`, `--english`, `--translation`, and `--output`.
For `texts`, output is one archive file; for `quests`, it is a directory of all
canonical English quests, localized where safe. A missing translation directory
is supported. `--source-encoding` enables strict translation decoding;
`--target-encoding` defaults to the source encoding and requires
`--source-encoding`. Invalid translation bytes or unrepresentable characters
fall back per entry/file. Omitting both options preserves translation bytes.
Exit status is zero on success (including fallback), nonzero on fatal errors.

To switch the installed mod language, copy **both** `res/*.res` and `maps/*.mq`
from `lang-packs/<LANGUAGE>/` into the matching mod directories, overwriting the
existing files. Reapply both manually after an updater run.
The launcher's Polish UI code is `pl`; the mod's Polish asset code is `pol`.
Changing the launcher UI language alone does not switch mod assets.

Isolated localization tests (no external tools required):

```bash
python3 -m unittest discover -s bin -p 'test_pack_language.py'
```

### Developed by Kyr4l
