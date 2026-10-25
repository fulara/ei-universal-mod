"""Isolated overlay tests; real archive packing belongs to the release smoke test."""

from contextlib import redirect_stderr
import importlib.util
import io
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location("pack_language", Path(__file__).with_name("pack_language.py"))
pack = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(pack)


class LanguagePackingTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.english = self.root / "eng"
        self.translation = self.root / "pol"
        self.english.mkdir()
        self.translation.mkdir()
        self.messages = io.StringIO()
        self.capture = redirect_stderr(self.messages)
        self.capture.__enter__()
        self.addCleanup(self.capture.__exit__, None, None, None)

    def file(self, root, name, data):
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        os.utime(path, (1500000000, 1500000000))
        return path

    def test_missing_empty_extra_and_technical_keys(self):
        english = {b"string missing": b"Current", b"string empty": b"Not empty",
                   b"string good": b"Hello", b"string version_name": b"Current version",
                   b"map-lmp.txt": b"Current metadata"}
        translation = {b"string empty": b" \r\n", b"string good": b"Czesc",
                       b"string obsolete": b"Old", b"map-lmp.txt": b"STALE"}
        merged = pack.overlay_texts(english, translation)
        self.assertEqual(set(merged), set(english))
        self.assertEqual(merged[b"string missing"], b"Current")
        self.assertEqual(merged[b"string empty"], b"Not empty")
        self.assertEqual(merged[b"string good"], b"Czesc")
        self.assertEqual(merged[b"map-lmp.txt"], b"Current metadata")
        warnings = self.messages.getvalue()
        self.assertIn("stale text key", warnings)
        self.assertIn("empty translation", warnings)
        self.assertNotIn("version_name", warnings)
        self.assertNotIn("map-lmp.txt", warnings)

    def test_grouped_escaping_and_exact_newlines(self):
        entries = {b"string first": b"=== literal\r\n\\path\r\nlast\r\n",
                   b"string empty": b"", b"string final": b"no newline"}
        self.assertEqual(pack.parse_group(pack.write_group(entries)), entries)
        crlf = b"# um-texts 1\r\n=== string x\r\n\\=== text\r\n\\\\path\r\n\r\n"
        self.assertEqual(pack.parse_group(crlf), {b"string x": b"=== text\r\n\\path\r\n"})
        # Even a lone backslash escape is consumed, as in textgroups::Parse.
        self.assertEqual(pack.parse_group(b"=== string x\n\\abc\n"), {b"string x": b"abc"})

    def test_loose_overrides_grouped_and_tmp_is_ignored(self):
        self.file(self.translation, "STRING.umtexts", pack.write_group({b"string x": b"grouped"}))
        self.file(self.translation, "string x", b"loose")
        self.file(self.translation, "ignored.tmp", b"temporary")
        self.assertEqual(pack.load_texts(self.translation, True), {b"string x": b"loose"})

    def test_malformed_translation_falls_back_but_invalid_english_fails(self):
        self.file(self.translation, "STRING.umtexts", b"broken group")
        self.file(self.translation, "string x", b"bad\0text")
        translated = pack.load_texts(self.translation, True)
        self.assertEqual(pack.overlay_texts({b"string x": b"Current"}, translated),
                         {b"string x": b"Current"})
        self.file(self.english, "string x", b"bad\0English")
        with self.assertRaises(ValueError):
            pack.load_texts(self.english)

    def test_polish_encoding_and_mixed_english_fallback(self):
        polish = "Zażółć gęślą jaźń"
        self.file(self.translation, "STRING.umtexts", pack.write_group({b"string x": polish.encode("utf-8")}))
        self.file(self.english, "string utf8", "Traveller’s story".encode("utf-8"))
        self.file(self.english, "string legacy", "Traveller’s story".encode("cp1252"))
        self.file(self.english, "map-lmp.txt", b"technical\x81")
        self.file(self.english, "string upstream", "Upstream � punctuation".encode("utf-8"))
        translated = pack.load_texts(self.translation, True, "utf-8", "cp1250")
        self.assertEqual(translated[b"string x"], polish.encode("cp1250"))
        english = pack.load_texts(self.english)
        self.assertEqual(english[b"string utf8"], "Traveller’s story".encode("utf-8"))
        self.assertEqual(english[b"string legacy"], "Traveller’s story".encode("cp1252"))
        self.assertEqual(english[b"map-lmp.txt"], b"technical\x81")
        self.assertEqual(english[b"string upstream"], "Upstream � punctuation".encode("utf-8"))
        self.assertEqual(pack.load_texts(self.translation, True)[b"string x"], polish.encode("utf-8"))

    def test_bad_translation_encoding_is_per_entry_fallback(self):
        self.file(self.translation, "STRING.umtexts", pack.write_group({b"string good": "Żaba".encode(),
                                                                       b"string bad": b"\xff"}))
        translated = pack.load_texts(self.translation, True, "utf-8", "cp1250")
        merged = pack.overlay_texts({b"string good": b"Toad", b"string bad": b"Current"}, translated)
        self.assertEqual(merged[b"string good"], "Żaba".encode("cp1250"))
        self.assertEqual(merged[b"string bad"], b"Current")
        self.assertIsNone(pack.encode_translation("😀".encode(), "emoji", "utf-8", "cp1250"))

    def test_format_placeholders_are_protected(self):
        self.assertEqual(pack.select_text(b"Count: %d", b"Liczba: %s", "count"), b"Count: %d")
        self.assertEqual(pack.select_text(b"Count: %d", b"Liczba: %d", "count"), b"Liczba: %d")

    def test_archived_briefing_controls_stay_current(self):
        key = b"briefing intro"
        original = b"Meet the smith\n#phrase Stark 1\nWelcome\n"
        stale = b"Poznaj kowala\n#phrase Hero 99\nWitaj\n"
        self.assertEqual(pack.overlay_texts({key: original}, {key: stale})[key], original)
        valid = b"Poznaj kowala\r\n#phrase Stark 1\r\nWitaj\r\n"
        self.assertEqual(pack.overlay_texts({key: original}, {key: valid})[key], valid)

    def quest(self, name, text=b"Current title\r\nCurrent description\r\n#subobj  1\r\nObjective\r\nDetails\r\n"):
        root = self.english / (name + "_mq")
        self.file(root, "quest " + name, text)
        self.file(root, "briefing " + name + "_1", b"Current briefing")
        self.file(root, name + "/quest.ini", b"[Quest]\r\nObjectives=1\r\n")
        self.file(root, name + "/map.txt", b"Current map\x81")
        return root

    def test_quest_controls_and_objective_counts(self):
        original = b"Title\nDescription\n#subobj  1\nFirst\nText\n#subobj  2\nSecond\nText\n#custom flag\n"
        valid = b"Tytul\r\nOpis\r\n#subobj  1\r\nPierwszy\r\nTekst\r\n#subobj  2\r\nDrugi\r\nTekst\r\n#custom flag\r\n"
        self.assertEqual(pack.select_text(original, valid, "quest", quest=True), valid)
        for invalid in (valid.replace(b"#subobj  2", b"#subobj  3"),
                        valid.replace(b"#subobj  2\r\n", b""),
                        valid + b"#subobj  4\r\nExtra\r\n",
                        valid.replace(b"#custom flag", b"#custom stale")):
            with self.subTest(invalid=invalid):
                self.assertEqual(pack.select_text(original, invalid, "quest", quest=True), original)

    def test_stage_includes_missing_quests_and_ignores_translated_metadata(self):
        self.quest("q1")
        self.quest("q2")
        pl = self.translation / "q1_mq"
        self.file(pl, "quest q1", "Tytuł\nOpis\n#subobj  1\nCel\nSzczegóły\n".encode())
        self.file(pl, "q1/quest.ini", b"STALE OBJECTIVES=99")
        self.file(pl, "q1/map.txt", b"STALE MAP")
        self.file(pl, "quest obsolete", b"STALE TEXT")
        self.file(self.translation / "obsolete_mq", "quest obsolete", b"old")
        stage = self.root / "stage"
        stage.mkdir()
        quests, sources = pack.stage_quests(self.english, self.translation, stage, "utf-8", "cp1250")
        self.assertEqual([q.name for q in quests], ["q1_mq", "q2_mq"])
        self.assertEqual(len(sources), 2)
        self.assertEqual((stage / "q1_mq/q1/quest.ini").read_bytes(), b"[Quest]\r\nObjectives=1\r\n")
        self.assertEqual((stage / "q1_mq/q1/map.txt").read_bytes(), b"Current map\x81")
        self.assertEqual((stage / "q1_mq/quest q1").read_bytes(),
                         "Tytuł\nOpis\n#subobj  1\nCel\nSzczegóły\n".encode("cp1250"))
        self.assertEqual((stage / "q2_mq/quest q2").read_bytes(),
                         (self.english / "q2_mq/quest q2").read_bytes())
        self.assertEqual((stage / "q1_mq/briefing q1_1").read_bytes(), b"Current briefing")
        self.assertFalse((stage / "q1_mq/quest obsolete").exists())
        self.assertFalse((stage / "obsolete_mq").exists())
        self.assertEqual((stage / "q1_mq/quest q1").stat().st_mtime_ns, 1500000000000000000)

    def test_quest_staging_falls_back_on_stale_control_and_bad_encoding(self):
        english = self.quest("q1")
        pl = self.translation / "q1_mq"
        self.file(pl, "quest q1", b"Old\nOld\n#subobj  9\nWrong\nWrong\n")
        self.file(pl, "briefing q1_1", b"\xff")
        stage = self.root / "stage"
        stage.mkdir()
        pack.stage_quests(self.english, self.translation, stage, "utf-8", "cp1250")
        for name in ("quest q1", "briefing q1_1"):
            self.assertEqual((stage / "q1_mq" / name).read_bytes(), (english / name).read_bytes())

    def test_cli_canonical_and_tool_failures_return_nonzero(self):
        args = ["texts", "--tool", str(self.root / "missing-tool"), "--english", str(self.english),
                "--translation", str(self.translation), "--output", str(self.root / "texts.res")]
        self.assertEqual(pack.main(args), 1)
        self.file(self.english, "string x", b"Current")
        with patch.object(pack, "run_tool", side_effect=subprocess.CalledProcessError(7, "tool")):
            self.assertEqual(pack.main(args), 1)
        self.assertFalse((self.root / "texts.res").exists())


if __name__ == "__main__":
    unittest.main()
