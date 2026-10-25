# Curse of the Lost Soul reference assets

This directory contains reference assets for the original **Evil Islands: Curse of the Lost Soul** (COTLS), not **Lost in Astral** (LIA). LIA references are kept separately in [`../lia-vanilla`](../lia-vanilla).

These are reference/source files. EIStarter-UM does not automatically install them, and their presence does not provide a complete Polish translation of Universal Mod.

## Polish resources

The `pol-` prefix identifies Polish resources; it is not part of the game's archive filenames.

| Directory | Game archive | Contents |
| --- | --- | --- |
| [`pol-menus_res`](pol-menus_res) | `menus.res` | 12 files: graphical menu labels, textures, configuration and models |
| [`pol-speech_res`](pol-speech_res) | `speech.res` | 2,974 MP3 dialogue recordings |
| [`pol-sfx_res`](pol-sfx_res) | `sfx.res` | 4,134 WAV effects and character acknowledgements; not every file is speech |
| [`pol-texts_res`](pol-texts_res) | `texts.res` | 3,170 base-game text entries, grouped as `.umtexts` |
| [`pol-textslmp_res`](pol-textslmp_res) | `textslmp.res` | 397 multiplayer text entries, grouped as `.umtexts` |

The menu, speech and SFX directories were extracted from a locally preserved `Evil Islands - Spolszczenie/Res` package using um-multitool 1.15.2. Their 7,120 payloads were checked byte-for-byte against the source archives. Inner paths, including the dialogue subdirectories, must not be flattened.

The Polish text directories already existed in this repository and were retained. The preserved package's `textslmp.res` matches all 397 existing payloads. Its `texts.res` differs in 35 entries, including apparent typos in the preserved package; it was not used to overwrite the existing reference text.

Text payloads retain their original encoding, including CP1250 Polish text. Do not blindly re-save `.umtexts` files as UTF-8. See the [grouped text format](../../../docs/file-formats/umtexts-format.md).

## Rebuild the game archives

Use [um-multitool](../../../tools/um-multitool/README.md), not a ZIP archiver. `restool --pack` rebuilds the raw asset directories; `texts --pack` rebuilds the grouped text directories.

The following is a POSIX-shell example, run from the repository root with `um-multitool` on `PATH`. It writes all output to a new temporary directory, leaving the reference files unchanged:

```sh
REF="extra-assets/reference-assets/cotls-vanilla"
OUT="$(mktemp -d "${TMPDIR:-/tmp}/ei-cotls-pl.XXXXXX")"

um-multitool restool --pack "$REF/pol-menus_res" -o "$OUT/menus.res"
um-multitool restool --pack "$REF/pol-speech_res" -o "$OUT/speech.res"
um-multitool restool --pack "$REF/pol-sfx_res" -o "$OUT/sfx.res"
um-multitool texts --pack "$REF/pol-texts_res" -o "$OUT/texts.res"
um-multitool texts --pack "$REF/pol-textslmp_res" -o "$OUT/textslmp.res"

printf 'Rebuilt archives: %s\n' "$OUT"
```

On Windows, use `um-multitool.exe` with the same subcommands and Windows paths. On macOS, the Windows executable can be run inside a CrossOver bottle; the example above is not a native macOS build of the tool.

Rebuilt archive containers need not be byte-identical to the original `.res` files: packing can deduplicate payloads and change metadata. Validation compared entry paths under the format's case-insensitive lookup and every payload's length and SHA-256. On the tested case-insensitive filesystem, 31 mixed `Acks`/`acks` path spellings were canonicalized during the SFX roundtrip; payloads and lookup keys were unchanged.

## Scope and installation caution

For an owned base-game installation, close the game and back up the original archives before replacing selected files in its `Res` directory. These are **base-game** resources: do not overwrite `Universal-Mod/res`, its gameplay databases or its maps with them. New UM content requires its own compatible language pack.

The graphical menu and Polish accented text were checked separately in a CrossOver installation. Extraction and archive rebuilding were checked for all new assets, and one MP3 and one WAV were decoded to PCM. Full in-game audio playback was not verified. Choosing a Polish locale does not translate Russian mod strings and can make mixed-encoding resources unreadable.

The assets keep their owners' terms; their inclusion does not place the game's recordings, textures or translations under the tools' GPL license. See the [asset notice](../../ASSETS_NOTICE.md) and [licensing information](../../../docs/licensing.md).
