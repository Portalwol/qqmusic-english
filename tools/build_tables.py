"""Turn translations/*.json into the files the add-on reads: strings.tsv, web_dict.js, web.js.

usage: python -X utf8 tools/build_tables.py <out dir>
Every translations/*.json is merged in name order (later files win), so a local
override file such as translations/zz-local.json can sit next to zh-en.json.
Identity mappings are dropped.
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
# Looked up by name in code (fonts, files), so they must stay Chinese.
KEEP = re.compile(r"^(宋体|新宋体|黑体|楷体|仿宋|幼圆|微软雅黑|微软正黑体)[;；]?$|\.(lnk|exe|dll|ini|xml|txt|json|png|jpg|gif|mp3|flac)$", re.I)
HAN = re.compile(r"[\u4e00-\u9fff]")
FIELD = re.compile(r"%[-0-9.]*(?:l|ll|I64)?[dius]")


def esc(s):
    return s.replace("\\", "\\\\").replace("\t", "\\t").replace("\r", "\\r").replace("\n", "\\n")


def tables():
    table = {}
    for f in sorted((ROOT / "translations").glob("*.json")):
        table.update(json.loads(f.read_text(encoding="utf-8")))
    return {k: v for k, v in table.items() if k != v and v.strip() and not KEEP.search(k)}


if __name__ == "__main__":
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    table = tables()
    rows = [f"{esc(k)}\t{esc(v)}" for k, v in sorted(table.items())]
    (out / "strings.tsv").write_text("\n".join(rows) + "\n", encoding="utf-8")
    # Web pages match whole (trimmed) text only: no printf templates, no single characters.
    web = {}
    for k, v in sorted(table.items()):
        k2 = k.strip()
        if len(k2) >= 2 and HAN.search(k2) and "%" not in k2 and "&" not in v:
            web.setdefault(k2, v.strip())
    tpl = [[k.strip(), v.strip()] for k, v in sorted(table.items())
           if FIELD.search(k) and len(FIELD.findall(k)) == len(FIELD.findall(v)) and "&" not in v
           and len(HAN.findall(FIELD.sub("", k))) >= 2]
    dict_js = "window.__qqenDict=" + json.dumps(web, ensure_ascii=False, separators=(",", ":")) + ";\n"
    dict_js += "window.__qqenTpl=" + json.dumps(tpl, ensure_ascii=False, separators=(",", ":")) + ";\n"
    (out / "web_dict.js").write_text(dict_js, encoding="utf-8")
    (out / "web.js").write_text((ROOT / "web" / "web.js").read_text(encoding="utf-8"), encoding="utf-8")
    print(f"{len(rows)} translations, {len(web)} for web pages -> {out}")
