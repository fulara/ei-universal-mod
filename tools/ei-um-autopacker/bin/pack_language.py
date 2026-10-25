#!/usr/bin/env python3
"""Overlay language sources on current English, then use um-multitool to pack."""

import argparse
from collections import Counter
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

TECHNICAL = {b"string version_name", b"map-lmp.txt"}
PRINTF = re.compile(rb"%(?:\d+\$)?[-+#0 ]*(?:\d+|\*)?(?:\.(?:\d+|\*))?(?:hh|ll|[hljztL])?[diuoxXfFeEgGaAcspn%]")


def warn(message):
    print(f"warning: {message}", file=sys.stderr)


def valid_key(key):
    return bool(key) and not any(c in key for c in (b"\0", b"\n", b"\r"))

def encode_translation(data, label, source_encoding, target_encoding):
    if not source_encoding:
        return data
    try:
        return data.decode(source_encoding).encode(target_encoding)
    except UnicodeError as error:
        warn(f"{label}: malformed/unencodable translation ({error}); using English")
        return None


def parse_group(data):
    """Match textgroups::Parse, including CRLF and its one formatting newline."""
    entries = {}
    name = None
    body = bytearray()

    def flush():
        if name is not None:
            payload = bytes(body)
            if payload.endswith(b"\r\n"):
                payload = payload[:-2]
            elif payload.endswith(b"\n"):
                payload = payload[:-1]
            entries[name] = payload
            body.clear()

    # splitlines() also splits CR and other control bytes, unlike the C++ parser.
    lines = data.split(b"\n")
    for index, line in enumerate(lines):
        if index == len(lines) - 1 and not line:
            break
        if line.startswith(b"=== "):
            flush()
            name = line[4:]
            if name.endswith(b"\r"):
                name = name[:-1]
        elif name is not None:
            body.extend(line[1:] if line.startswith(b"\\") else line)
            if index < len(lines) - 1:
                body.extend(b"\n")
    flush()
    return entries


def write_group(entries):
    result = bytearray(b"# um-texts 1\n")
    for key, body in sorted(entries.items()):
        result.extend(b"=== " + key + b"\n")
        result.extend(b"\n".join((b"\\" + line if line.startswith((b"===", b"\\")) else line)
                                  for line in body.split(b"\n")))
        result.extend(b"\n")
    return bytes(result)


def load_texts(directory, translation=False, source_encoding=None, target_encoding=None):
    if not directory.exists() and translation:
        warn(f"{directory}: missing translation directory; using English")
        return {}
    if not directory.is_dir():
        raise ValueError(f"not a source directory: {directory}")
    entries = {}
    files = sorted(p for p in directory.iterdir() if p.is_file())
    for grouped in (True, False):
        for path in files:
            if (path.suffix == ".umtexts") != grouped:
                continue
            if not grouped and len(path.name) > 4 and path.name.endswith(".tmp"):
                continue
            data = path.read_bytes()
            found = parse_group(data) if grouped else {os.fsencode(path.name): data}
            if grouped and not found:
                if not translation:
                    raise ValueError(f"{path}: grouped English source contains no entries")
                warn(f"{path}: malformed grouped translation; using English")
            for key, body in found.items():
                if not valid_key(key) or b"\0" in body:
                    if not translation:
                        raise ValueError(f"{path}: invalid English entry {key!r}")
                    warn(f"{path}: malformed translation {key!r}; using English")
                    # An invalid loose override must not revive a grouped translation.
                    entries[key] = None
                else:
                    if translation and key not in TECHNICAL:
                        body = encode_translation(body, f"{path}: {key!r}",
                                                  source_encoding, target_encoding)
                    entries[key] = body
    if not entries and not translation:
        raise ValueError(f"{directory}: English keyset is empty")
    return entries


def placeholders(data):
    return Counter(PRINTF.findall(data))


def select_text(english, translated, label, technical=False, quest=False):
    if technical:
        return english
    reason = None
    if translated is None:
        reason = "missing or malformed translation"
    elif not translated.strip():
        reason = "empty translation"
    elif b"\0" in translated:
        reason = "malformed translation"
    elif placeholders(english) != placeholders(translated):
        reason = "changed format placeholders"
    elif quest and controls(english) != controls(translated):
        reason = "stale quest controls/objective IDs"
    if reason:
        warn(f"{label}: {reason}; using English")
        return english
    return translated


def controls(data):
    """Every directive, in order, with only the line ending normalized."""
    return [line for line in data.replace(b"\r\n", b"\n").split(b"\n")
            if line.lstrip().startswith(b"#")]


def overlay_texts(english, translation):
    for key in sorted(translation.keys() - english.keys()):
        warn(f"stale text key {key!r}; omitted")
    return {key: select_text(body, translation.get(key), repr(key), key in TECHNICAL,
                             quest=key.lower().startswith(b"briefing "))
            for key, body in english.items()}


def put_file(path, data, source):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    stat = source.stat()
    os.utime(path, ns=(stat.st_atime_ns, stat.st_mtime_ns))


def stage_quests(english, translation, stage, source_encoding=None, target_encoding=None):
    if not english.is_dir():
        raise ValueError(f"not an English quest directory: {english}")
    if translation.exists() and not translation.is_dir():
        raise ValueError(f"not a translation directory: {translation}")
    quests = sorted(p for p in english.iterdir() if p.is_dir() and p.name.endswith("_mq"))
    if not quests:
        raise ValueError(f"{english}: no canonical English quests")
    canonical = {p.name for p in quests}
    if translation.is_dir():
        for extra in sorted(translation.iterdir()):
            if extra.is_dir() and extra.name.endswith("_mq") and extra.name not in canonical:
                warn(f"{extra}: stale translated quest; omitted")
    ini_sources = []
    for quest in quests:
        translated_dir = translation / quest.name
        if not translated_dir.exists():
            warn(f"{translated_dir}: missing translated quest; using English")
        elif not translated_dir.is_dir():
            raise ValueError(f"not a translated quest directory: {translated_dir}")
        destination = stage / quest.name
        shutil.copytree(quest, destination, copy_function=shutil.copy2)
        human_names = set()
        quest_inis = []
        for source in sorted(quest.rglob("*")):
            if not source.is_file():
                continue
            relative = source.relative_to(quest)
            if source.name == "quest.ini":
                quest_inis.append(source)
                ini_sources.append((source, destination / relative.with_suffix(".reg")))
            if len(relative.parts) != 1 or not source.name.startswith(("quest ", "briefing ")):
                continue
            human_names.add(source.name)
            original = source.read_bytes()
            if b"\0" in original or not original.strip():
                raise ValueError(f"{source}: invalid English quest text")
            translated_path = translated_dir / source.name
            translated = None
            if translated_path.exists():
                translated = translated_path.read_bytes()
                translated = encode_translation(translated, str(translated_path),
                                                source_encoding, target_encoding)
            selected = select_text(original, translated, str(translated_path), quest=True)
            put_file(destination / relative, selected, source)
        if len(quest_inis) != 1:
            raise ValueError(f"{quest}: expected exactly one current quest.ini, found {len(quest_inis)}")
        if translated_dir.is_dir():
            for extra in sorted(translated_dir.iterdir()):
                if extra.is_file() and extra.name.startswith(("quest ", "briefing ")) and extra.name not in human_names:
                    warn(f"{extra}: stale translated text; omitted")
    return quests, ini_sources


def run_tool(tool, *args):
    subprocess.run([str(tool), *map(str, args)], check=True)

def pack_texts(args):
    english = load_texts(args.english)
    translation = load_texts(args.translation, True, args.source_encoding, args.target_encoding)
    entries = overlay_texts(english, translation)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="um-language-texts-") as temporary:
        stage = Path(temporary)
        # Grouped staging also avoids filesystem restrictions on archive entry names.
        put_file(stage / "TEXTS.umtexts", write_group(entries), args.english)
        run_tool(args.tool, "texts", "--pack", stage, "-o", args.output)
        if not args.output.is_file():
            raise ValueError(f"multitool did not produce {args.output}")


def pack_quests(args):
    args.output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="um-language-quests-") as temporary:
        root = Path(temporary)
        stage = root / "quests"
        stage.mkdir()
        quests, sources = stage_quests(args.english, args.translation, stage,
                                       args.source_encoding, args.target_encoding)
        # inireg's directory API is non-recursive. Flatten with unique names for one batch.
        convert = root / "convert"
        convert.mkdir()
        for index, (source, _) in enumerate(sources):
            shutil.copy2(source, convert / f"{index}.ini")
        run_tool(args.tool, "inireg", "--ini2reg", "-d", convert, "-m")
        for index, (source, destination) in enumerate(sources):
            put_file(destination, (convert / f"{index}.reg").read_bytes(), source)
        packed = root / "packed"
        packed.mkdir()
        run_tool(args.tool, "restool", "--pack", "-d", stage, "-o", packed, "-m", "-e", "quest.ini")
        for quest in quests:
            name = quest.name[:-3] + ".mq"
            shutil.copy2(packed / name, args.output / name)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    for command in ("texts", "quests"):
        sub = commands.add_parser(command)
        sub.add_argument("--tool", required=True, type=Path)
        sub.add_argument("--english", required=True, type=Path)
        sub.add_argument("--translation", required=True, type=Path)
        sub.add_argument("--output", required=True, type=Path,
                         help="archive file for texts; destination directory for quests")
        sub.add_argument("--source-encoding", help="translation source encoding (default: preserve bytes)")
        sub.add_argument("--target-encoding", help="translation output encoding (default: source encoding)")
    args = parser.parse_args(argv)
    if args.target_encoding and not args.source_encoding:
        parser.error("--target-encoding requires --source-encoding")
    args.target_encoding = args.target_encoding or args.source_encoding
    args.tool = args.tool.resolve()
    try:
        (pack_texts if args.command == "texts" else pack_quests)(args)
    except (OSError, ValueError, LookupError, subprocess.CalledProcessError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
