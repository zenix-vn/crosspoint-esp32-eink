"""Builds vocabulary decks (.tsv) for the CrossPoint trainer.

Word lists, levels and frequency come from ECDICT (ecdict.csv); Vietnamese
meanings, phonetics and examples from the OVDP English-Vietnamese StarDict
(star_anhviet.*). Output: decks/<name>.tsv, most frequent words first.
"""

import csv
import gzip
import os
import re
import struct
import sys

sys.stdout.reconfigure(encoding="utf-8")
csv.field_size_limit(10_000_000)
# Usage: build_decks.py DATA_DIR [OUT_DIR]
#   DATA_DIR holds ecdict.csv (github.com/skywind3000/ECDICT) and star_anhviet.idx /
#   star_anhviet.dict.dz (github.com/dynamotn/stardict-vi, en-vi).
HERE = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.getcwd()
OUT = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else os.path.join(HERE, "decks")

POS = {
    "danh từ": "n", "tính từ": "adj", "phó từ": "adv", "trạng từ": "adv", "ngoại động từ": "v",
    "nội động từ": "v", "động từ": "v", "giới từ": "prep", "liên từ": "conj", "đại từ": "pron",
    "thán từ": "interj", "mạo từ": "art", "số từ": "num",
}
# Senses worth skipping on a flash card: archaic, slang, joking registers.
SKIP_SENSE = re.compile(r"^\((từ cổ|từ hiếm|nói trại|từ lóng|đùa|tục|thô tục|thông tục|tech|econ|từ mỹ,nghĩa mỹ\) \(từ lóng)", re.I)
# Specialist glossaries mixed into the dictionary: used only when nothing else exists.
SPECIALIST = re.compile(r"^-?\s*\((tech|econ)\)", re.I | re.M)


def load_anhviet():
    idx = open(os.path.join(HERE, "star_anhviet.idx"), "rb").read()
    data = gzip.open(os.path.join(HERE, "star_anhviet.dict.dz")).read()
    entries = {}
    i = 0
    while i < len(idx):
        j = idx.index(b"\0", i)
        word = idx[i:j].decode("utf-8", "replace")
        off, size = struct.unpack(">II", idx[j + 1:j + 9])
        entries.setdefault(word.lower(), []).append((off, size))
        i = j + 9
    return entries, data


def ipa(raw):
    raw = raw.strip().strip("/").strip()
    if not raw:
        return ""
    raw = raw.replace("'", "ˈ").replace(":", "ː")
    raw = re.sub(r"^,|(?<=[\s.])\s*,", "ˌ", raw)
    return "/" + raw + "/"


def clean(text, limit):
    text = re.sub(r"\s+", " ", text.replace("\t", " ")).strip(" ;,")
    if len(text) > limit:
        cut = text[:limit]
        text = cut[: cut.rfind(" ")] + "…" if " " in cut else cut + "…"
    return text


def parse_entry(text):
    lines = [l.strip() for l in text.split("\n") if l.strip()]
    if not lines or not lines[0].startswith("@"):
        return None
    head = lines[0]
    m = re.search(r"/([^/]+)/", head)
    phon = ipa(m.group(1)) if m else ""
    groups = []  # [pos, [meanings]]
    example = ""
    for line in lines[1:]:
        if line.startswith("@") or line.startswith("!"):
            break
        if line.startswith("*"):
            groups.append([line.lstrip("* ").strip().lower(), []])
            continue
        if line.startswith("- ") or (line.startswith("-") and not line.startswith("--")):
            sense = line[1:].strip()
            if not sense or SKIP_SENSE.match(sense.lower()):
                continue
            if not groups:
                groups.append(["", []])
            groups[-1][1].append(sense)
            continue
        if line.startswith("=") and not example and groups and groups[-1][1]:
            en, _, vi = line[1:].partition("+")
            if en.strip() and vi.strip():
                example = f"{en.strip()} — {vi.strip()}"
    parts = []
    taken = 0
    for pos, senses in groups:
        if not senses or taken >= 3:
            continue
        keep = senses[: 2 if taken == 0 else 1]
        taken += len(keep)
        label = POS.get(pos, "")
        parts.append((f"({label}) " if label else "") + "; ".join(keep))
        if len(parts) == 2:
            break
    meaning = clean(" | ".join(parts), 140)
    if not meaning:
        return None
    return phon, meaning, clean(example, 170)


def main():
    entries, data = load_anhviet()
    print("anhviet entries", len(entries))

    words = []
    with open(os.path.join(HERE, "ecdict.csv"), encoding="utf-8") as f:
        for row in csv.DictReader(f):
            w = row["word"].strip()
            if not re.fullmatch(r"[a-z][a-z'-]*[a-z]|[a-z]", w):
                continue
            frq = int(row["frq"] or 0)
            bnc = int(row["bnc"] or 0)
            rank = frq if frq > 0 else (bnc if bnc > 0 else 10**6)
            words.append({
                "word": w, "phon": row["phonetic"], "tags": set((row["tag"] or "").split()),
                "oxford": row["oxford"] == "1", "rank": rank,
            })
    print("ecdict single words", len(words))

    cards = {}
    for item in words:
        hits = entries.get(item["word"])
        if not hits:
            continue
        # Among same-named entries prefer the general one: part-of-speech
        # blocks, no specialist label, the most content.
        best = None
        best_score = -1
        for off, size in hits:
            text = data[off:off + size].decode("utf-8", "replace")
            parsed = parse_entry(text)
            if not parsed:
                continue
            score = text.count("\n*") * 1000 + len(text) - (100000 if SPECIALIST.search(text) else 0)
            if score > best_score:
                best, best_score = parsed, score
        parsed = best
        if not parsed:
            continue
        phon, meaning, example = parsed
        if not phon and item["phon"]:
            phon = ipa(item["phon"])
        cards[item["word"]] = (item, phon, meaning, example)
    print("cards with Vietnamese meaning", len(cards))

    decks = [
        ("1. Oxford 3000", lambda it: it["oxford"], None,
         "The Oxford 3000 core words, most frequent first."),
        ("2. 5000 từ thông dụng", lambda it: it["rank"] < 10**6, 5000,
         "The 5000 most frequent English words."),
        ("3. Tiếng Anh THCS", lambda it: "zk" in it["tags"], None, "Lower secondary school vocabulary."),
        ("4. Tiếng Anh THPT", lambda it: "gk" in it["tags"], None, "Upper secondary / university entrance vocabulary."),
        ("5. Trung cấp", lambda it: "cet4" in it["tags"], None, "Intermediate (CET-4 list)."),
        ("6. Nâng cao", lambda it: "cet6" in it["tags"], None, "Upper intermediate (CET-6 list)."),
        ("7. IELTS", lambda it: "ielts" in it["tags"], None, "IELTS vocabulary."),
        ("8. TOEFL", lambda it: "toefl" in it["tags"], None, "TOEFL vocabulary."),
        ("9. GRE học thuật", lambda it: "gre" in it["tags"], None, "Advanced academic (GRE) vocabulary."),
    ]
    os.makedirs(OUT, exist_ok=True)
    for name, keep, cap, about in decks:
        chosen = sorted((c for c in cards.values() if keep(c[0])), key=lambda c: (c[0]["rank"], c[0]["word"]))
        if cap:
            chosen = chosen[:cap]
        path = os.path.join(OUT, name + ".tsv")
        with open(path, "w", encoding="utf-8", newline="\n") as out:
            out.write(f"# {name}: {about} {len(chosen)} words.\n")
            out.write("# word\tIPA\tmeaning\texample\n")
            for item, phon, meaning, example in chosen:
                out.write("\t".join([item["word"], phon, meaning, example]) + "\n")
        print(f"{name}: {len(chosen)} words, {os.path.getsize(path) // 1024} KB")


if __name__ == "__main__":
    main()
