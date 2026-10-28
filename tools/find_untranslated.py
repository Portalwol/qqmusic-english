"""Sweep every QQ Music binary for UTF-16 Chinese strings that no table translates yet.

usage: python -X utf8 tools/find_untranslated.py <QQMusic dir> <out.json>
Writes {string: [files...]} for strings that look like UI text and are not in
translations/*.json yet. Run it after a QQ Music update to see what is new. It
finds strings in the program files only; text that reaches the screen untranslated
is also logged by the add-on itself (missing.txt, missing_web.txt).
"""
import json
import re
import sys
from pathlib import Path

PUNCT = set("，。：；！？、（）“”‘’《》【】…—·～％＋－／「」『』〔〕〈〉￥")
HAN = re.compile(r"[一-鿿]")
# Log lines, code, paths, SQL and similar developer text.
LOGLIKE = re.compile(
    r"[A-Za-z_]{3,}\(|::|\[%|=\s*%|\\\\|/\w+/|【monitor】|<AsyncAPI>|\b(SELECT|INSERT|UPDATE|DELETE)\b|"
    r"\b(fail|error|ret|hr|err|code|uin|ptr|size)\s*[:=]|\bm_\w+|\bp\w*->|[{}]\s*$|^\s*[\[<]\w+[\]>]"
)


def ok_char(c):
    if c.isascii():
        return c.isprintable() or c in "\n\t"
    if c in PUNCT:
        return True
    try:
        b = c.encode("gb2312")
    except UnicodeEncodeError:
        return False
    return len(b) == 2 and 0xB0 <= b[0] <= 0xF7  # any GB2312 hanzi (levels 1 and 2)


def candidate(s):
    if not (2 <= len(s) <= 120):
        return False
    han = HAN.findall(s)
    if len(han) < 2:
        return False
    if not all(ok_char(c) for c in s):
        return False
    # Binary noise decodes to long runs of rare hanzi; real UI text is mostly level-1.
    lvl1 = sum(1 for c in han if 0xB0 <= c.encode("gb2312")[0] <= 0xD7)
    return lvl1 / len(han) >= 0.85 and not LOGLIKE.search(s)


def strings(data):
    found = set()
    for align in (0, 1):
        d = data[align:]
        d = d[: len(d) // 2 * 2]
        for piece in d.decode("utf-16le", errors="replace").split("\x00"):
            if candidate(piece):
                found.add(piece)
    return found


if __name__ == "__main__":
    app, out = Path(sys.argv[1]), Path(sys.argv[2])
    known = set()
    for t in (Path(__file__).resolve().parent.parent / "translations").glob("*.json"):
        known |= set(json.loads(t.read_text(encoding="utf-8")))
    hits = {}
    for f in sorted(app.rglob("*")):
        if f.suffix.lower() not in (".dll", ".exe") or "qmbrowser" in f.parts:
            continue
        for s in strings(f.read_bytes()):
            if s not in known and s.strip() not in known:
                hits.setdefault(s, []).append(str(f.relative_to(app)))
    out.write_text(json.dumps(dict(sorted(hits.items())), ensure_ascii=False, indent=0), encoding="utf-8")
    per = {}
    for s, fs in hits.items():
        for f in fs:
            per[f] = per.get(f, 0) + 1
    for f, n in sorted(per.items(), key=lambda x: -x[1])[:25]:
        print(f"{n:6} {f}")
    print(len(hits), "new strings")
