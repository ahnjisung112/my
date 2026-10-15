"""dwg2pdf.src.html (본문 조각) 을 완전한 HTML 문서로 만든다.

  python build_html.py                      -> dist/dwg2pdf.html          (CDN 에서 엔진을 받는 가벼운 버전)
  python build_html.py --offline            -> dist/dwg2pdf_offline.html  (엔진을 내장한 오프라인 버전, 약 15 MB)
  python build_html.py --site               -> site/dwg2pdf/ 폴더 (웹사이트에 그대로 올리는 버전: index.html + vendor/)

테스트용 옵션: --libredwg-base, --jspdf, --jszip 로 라이브러리 주소를 바꿀 수 있다.
"""
from __future__ import annotations

import argparse
import base64
import re
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE / "dwg2pdf.src.html"
NM = HERE / "node_modules"
LIBREDWG = NM / "@mlightcad" / "libredwg-web"
JSPDF_CDN = "https://cdnjs.cloudflare.com/ajax/libs/jspdf/2.5.1/jspdf.umd.min.js"
JSZIP_CDN = "https://cdnjs.cloudflare.com/ajax/libs/jszip/3.10.1/jszip.min.js"

HEAD = ('<!doctype html>\n<html lang="ko">\n<head>\n<meta charset="utf-8">\n'
        '<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">\n')


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--offline", action="store_true")
    ap.add_argument("--site", nargs="?", const=str(HERE / "site" / "dwg2pdf"), help="웹사이트 배포용 폴더 경로")
    ap.add_argument("--out")
    ap.add_argument("--libredwg-base")
    ap.add_argument("--jspdf", default=JSPDF_CDN)
    ap.add_argument("--jszip", default=JSZIP_CDN)
    a = ap.parse_args()

    if a.site:
        import shutil
        site = Path(a.site)
        vend = site / "vendor"
        (vend / "libredwg" / "dist").mkdir(parents=True, exist_ok=True)
        (vend / "libredwg" / "wasm").mkdir(parents=True, exist_ok=True)
        for sub in ("dist/libredwg-web.js", "wasm/libredwg-web.js", "wasm/libredwg-web.wasm"):
            shutil.copy2(LIBREDWG / sub, vend / "libredwg" / sub)
        shutil.copy2(NM / "jspdf" / "dist" / "jspdf.umd.min.js", vend / "jspdf.umd.min.js")
        shutil.copy2(NM / "jszip" / "dist" / "jszip.min.js", vend / "jszip.min.js")
        a.libredwg_base = "./vendor/libredwg/"
        a.jspdf = "./vendor/jspdf.umd.min.js"
        a.jszip = "./vendor/jszip.min.js"
        a.out = str(site / "index.html")

    body = SRC.read_text(encoding="utf-8")
    # <title>/<meta> 는 head 로 옮긴다
    title = re.search(r"<title>.*?</title>", body).group(0)
    body = body.replace(title, "", 1)
    body = re.sub(r'<meta name="claude:viewport-fit-sides"[^>]*>\n?', "", body)
    body = body.replace(JSPDF_CDN, a.jspdf).replace(JSZIP_CDN, a.jszip)
    head_extra = ""
    if a.libredwg_base:
        head_extra += f'<meta name="libredwg-base" content="{a.libredwg_base}">\n'

    if a.offline:
        def b64(p: Path) -> str:
            return base64.b64encode(p.read_bytes()).decode()
        dist = (LIBREDWG / "dist" / "libredwg-web.js").read_text(encoding="utf-8")
        glue = (LIBREDWG / "wasm" / "libredwg-web.js").read_text(encoding="utf-8")
        wasm_b64 = b64(LIBREDWG / "wasm" / "libredwg-web.wasm")
        jspdf = (NM / "jspdf" / "dist" / "jspdf.umd.min.js").read_text(encoding="utf-8")
        jszip = (NM / "jszip" / "dist" / "jszip.min.js").read_text(encoding="utf-8")
        body = re.sub(r'<script src="[^"]*jspdf[^"]*"></script>', lambda m: "<script>" + jspdf + "</script>", body)
        body = re.sub(r'<script src="[^"]*jszip[^"]*"></script>', lambda m: "<script>" + jszip + "</script>", body)
        # 링크 글꼴은 오프라인에서 의미가 없으므로 제거
        body = re.sub(r'<link rel="stylesheet" href="https://fonts.googleapis.com[^>]*>\n?', "", body)
        boot = (
            '<script id="v-dist" type="text/plain">' + base64.b64encode(dist.encode()).decode() + '</script>\n'
            '<script id="v-glue" type="text/plain">' + base64.b64encode(glue.encode()).decode() + '</script>\n'
            '<script id="v-wasm" type="text/plain">' + wasm_b64 + '</script>\n'
            '<script>\n(function(){\n'
            '  const b64 = (id) => Uint8Array.from(atob(document.getElementById(id).textContent), c => c.charCodeAt(0));\n'
            '  const txt = (id) => new TextDecoder().decode(b64(id));\n'
            '  const blobUrl = (data, type) => URL.createObjectURL(new Blob([data], {type}));\n'
            '  const wasmUrl = blobUrl(b64("v-wasm"), "application/wasm");\n'
            '  const glueUrl = blobUrl(txt("v-glue"), "text/javascript");\n'
            '  const dist = txt("v-dist").split("../wasm/libredwg-web.js").join(glueUrl);\n'
            '  window.DWG_VENDOR = { distUrl: blobUrl(dist, "text/javascript"), wasmUrl };\n'
            '  for (const id of ["v-dist","v-glue","v-wasm"]) document.getElementById(id).remove();\n'
            '})();\n</script>\n'
        )
        body = body.replace('<script type="module">', boot + '<script type="module">', 1)

    out = Path(a.out) if a.out else HERE / "dist" / ("dwg2pdf_offline.html" if a.offline else "dwg2pdf.html")
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(HEAD + title + "\n" + head_extra + "</head>\n<body>\n" + body + "\n</body>\n</html>\n", encoding="utf-8")
    print(f"written: {out} ({out.stat().st_size / 1e6:.1f} MB)")


if __name__ == "__main__":
    main()
