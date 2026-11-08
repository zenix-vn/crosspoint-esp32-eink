"""Builds Chinese vocabulary decks (.tsv) for the CrossPoint trainer.

One deck per HSK 3.0 level, most frequent words first:
  word (simplified) <TAB> pinyin <TAB> Vietnamese meaning <TAB> Hán Việt: <reading>

Word lists, levels, frequency, pinyin and traditional forms come from
complete-hsk-vocabulary (github.com/drkameleon/complete-hsk-vocabulary, MIT);
Vietnamese meanings from CVDICT (github.com/ph0ngp/CVDICT, CC BY-SA 4.0);
Sino-Vietnamese readings from hanviet-pinyin-wordlist
(github.com/ph0ngp/hanviet-pinyin-wordlist, MIT).
"""

import ast
import csv
import json
import os
import re
import sys

sys.stdout.reconfigure(encoding="utf-8")
# Usage: build_zh_decks.py DATA_DIR [OUT_DIR]
#   DATA_DIR holds complete.min.json, CVDICT.u8 and hanviet.csv.
HERE = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.getcwd()
OUT = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else os.path.join(HERE, "decks")

LEVELS = [("n1", "HSK 1"), ("n2", "HSK 2"), ("n3", "HSK 3"), ("n4", "HSK 4"), ("n5", "HSK 5"), ("n6", "HSK 6"),
          ("n7", "HSK 7-9")]
MAX_SENSES = 3
MAX_MEANING = 110
# Senses that tell a learner nothing on a flash card.
SKIP_SENSE = re.compile(r"^(CL:|biến thể|dạng cổ|dạng phồn thể|chữ cổ|xem |cũng viết là|họ \[|họ [A-ZĐ]|dùng trong |"
                        r"cách phát âm|phát âm Đài Loan|Đài Loan phát âm)", re.I)
# Readings no learner wants first: surnames, readings used only in one compound, variants.
MINOR_FORM = re.compile(r"^(surname|used in|variant of|old variant|unofficial variant|archaic|\(archaic\)|"
                        r"\(literary\)|\(of eyes\))", re.I)
# Function words are why 了, 着, 的 are in HSK 1: their particle reading comes first.
PARTICLE = re.compile(r"particle|marker", re.I)


def main_form(word):
    """The reading a learner meets first: 都 dōu, not the surname Dū; 了 le, not liǎo."""
    def score(form):
        meanings = form.get("m", [])
        s = min(len(meanings), 3)  # a rare reading with a long sense list must not win
        if not meanings or MINOR_FORM.match(meanings[0]):
            s -= 100
        if meanings and PARTICLE.search(meanings[0]):
            s += 5
        if form["i"]["y"][:1].isupper():
            s -= 50
        if meanings and meanings[0].startswith("(bound form)"):
            s -= 1
        return s
    forms = word["f"]
    return max(forms, key=lambda f: (score(f), -forms.index(f)))


def syllables(numeric):
    """'xue2 xi2' -> ['xue2', 'xi2'], with ü spelled 'v' so both sources agree."""
    return [s.lower().replace("u:", "v").replace("ü", "v") for s in numeric.split()]


def load_cvdict():
    entries = {}
    line_re = re.compile(r"^(\S+) (\S+) \[([^\]]*)\] /(.*)/$")
    with open(os.path.join(HERE, "CVDICT.u8"), encoding="utf-8") as f:
        for line in f:
            m = line_re.match(line.rstrip("\n"))
            if m:
                _, simp, pinyin, senses = m.groups()
                entries.setdefault(simp, []).append((syllables(pinyin), senses.split("/")))
    return entries


def load_hanviet():
    readings = {}
    with open(os.path.join(HERE, "hanviet.csv"), encoding="utf-8") as f:
        for row in csv.DictReader(f):
            values = ast.literal_eval(row["hanviet"])
            if values:
                key = (row["char"], row["pinyin"].lower().replace("u:", "v"))
                readings.setdefault(key, values[0])
                readings.setdefault((row["char"], "*"), values[0])
    return readings


def clean_sense(sense):
    # 牛郎織女|牛郎织女[niu2 lang3 zhi1 nu:3] -> 牛郎织女
    sense = re.sub(r"\S+\|(\S+?)\[[^\]]*\]", r"\1", sense)
    sense = re.sub(r"(\S+?)\[[^\]]*\]", r"\1", sense)
    sense = re.sub(r"\s*\[[A-Za-z: ]+\d[^\]]*\]", "", sense)  # "(phát âm thông tục [nei4])" -> "(phát âm thông tục)"
    return re.sub(r"\s+", " ", sense.replace("\t", " ")).strip(" ;,")


def meaning(word, sylls, cvdict):
    entries = cvdict.get(word, [])
    # Senses of the entry read the same way first, then of other readings.
    ordered = [e for e in entries if e[0] == sylls] + [e for e in entries if e[0] != sylls]
    senses = []
    for _, entry_senses in ordered:
        for s in map(clean_sense, entry_senses):
            # A sense with no Vietnamese word in it is a leftover classifier list ("个,位").
            if s and not SKIP_SENSE.match(s) and re.search(r"[A-Za-zÀ-ỹ]", s) and s not in senses:
                senses.append(s)
        if len(senses) >= MAX_SENSES:
            break
    text = ""
    for s in senses[:MAX_SENSES]:
        candidate = s if not text else text + "; " + s
        if len(candidate) > MAX_MEANING and text:
            break
        text = candidate
    return text[:MAX_MEANING]


def hanviet(traditional, sylls, readings):
    chars = list(traditional)
    if len(chars) != len(sylls):
        return ""
    out = []
    for ch, syl in zip(chars, sylls):
        r = readings.get((ch, syl)) or readings.get((ch, "*"))
        if not r:
            return ""
        out.append(r)
    return " ".join(out)


def main():
    words = json.load(open(os.path.join(HERE, "complete.min.json"), encoding="utf-8"))
    cvdict = load_cvdict()
    readings = load_hanviet()
    os.makedirs(OUT, exist_ok=True)
    han = set()
    for code, name in LEVELS:
        level = sorted((w for w in words if code in w.get("l", [])), key=lambda w: w.get("q", 10 ** 9))
        lines = [f"# {name}: từ vựng tiếng Trung (HSK 3.0), từ thông dụng trước.",
                 "# lang: zh",
                 "# Nguồn: complete-hsk-vocabulary (MIT), CVDICT (CC BY-SA 4.0), hanviet-pinyin-wordlist (MIT).",
                 "# từ | pinyin | nghĩa | Hán Việt"]
        missing = 0
        for w in level:
            form = main_form(w)
            sylls = syllables(form["i"]["n"])
            text = meaning(w["s"], sylls, cvdict)
            if not text:
                missing += 1
                continue
            pinyin = form["i"]["y"].replace("u:", "ü")
            hv = hanviet(form.get("t", w["s"]), sylls, readings)
            lines.append("\t".join([w["s"], pinyin, text, f"Hán Việt: {hv}" if hv else ""]).rstrip("\t"))
            han.update(c for c in w["s"] if 0x3400 <= ord(c) <= 0x9FFF or 0xF900 <= ord(c) <= 0xFAFF)
        path = os.path.join(OUT, name + ".tsv")
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(lines) + "\n")
        print(f"{name}: {len(lines) - 4} words ({missing} without a Vietnamese meaning)")
    write_han_intervals(sorted(han), os.path.join(OUT, "han_intervals.txt"))


def write_han_intervals(chars, path):
    """The decks' Han characters as code point runs, for the built-in Han font
    (fontconvert.py --intervals-file; see src/HanFont.h)."""
    runs = []
    for cp in map(ord, chars):
        if runs and cp == runs[-1][1] + 1:
            runs[-1][1] = cp
        else:
            runs.append([cp, cp])
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(f"# Han characters of the HSK decks: {len(chars)} characters in {len(runs)} runs.\n")
        f.writelines(f"0x{a:04X},0x{b:04X}\n" for a, b in runs)
    print(f"{path}: {len(chars)} Han characters")


if __name__ == "__main__":
    main()
