"""도면틀 하나를 PDF 로 렌더링한다 (ezdxf drawing add-on + PyMuPDF backend)."""
from __future__ import annotations

import re
from pathlib import Path
from typing import Optional

from ezdxf import bbox
from ezdxf.addons.drawing import Frontend, RenderContext, layout
from ezdxf.addons.drawing.config import BackgroundPolicy, ColorPolicy, Configuration, LineweightPolicy
from ezdxf.addons.drawing.pymupdf import PyMuPdfBackend
from ezdxf.fonts import fonts

from .frames import Frame, entities_in_frame

PAPER_MM = {
    "A0": (1189, 841),
    "A1": (841, 594),
    "A2": (594, 420),
    "A3": (420, 297),
    "A4": (297, 210),
}
KOREAN_FONT_CANDIDATES = [
    "malgun.ttf",          # Windows 맑은 고딕
    "NanumGothic.ttf",     # Linux fonts-nanum
    "NotoSansCJK-Regular.ttc",
    "NotoSansKR-Regular.otf",
    "AppleSDGothicNeo.ttc",
    "gulim.ttc",
]


def pick_fallback_font(preferred: Optional[str] = None) -> Optional[str]:
    fm = fonts.font_manager
    names = ([preferred] if preferred else []) + KOREAN_FONT_CANDIDATES
    for n in names:
        if fm.has_font(n):
            return n
    # 폰트 캐시가 오래됐을 수 있으니 한 번 다시 만든다
    fonts.build_system_font_cache()
    fonts.load()
    for n in names:
        if fm.has_font(n):
            return n
    return None


def substitute_missing_fonts(doc, fallback: Optional[str]) -> list[tuple[str, str]]:
    """시스템에 없는 글꼴을 쓰는 문자 스타일을 fallback 글꼴로 바꾼다."""
    if not fallback:
        return []
    fm = fonts.font_manager
    changed = []
    for style in doc.styles:
        font = style.dxf.font or ""
        if font and fm.has_font(font):
            continue
        style.dxf.font = fallback
        style.dxf.bigfont = ""
        changed.append((style.dxf.name, font))
    return changed


def page_for(frame: Frame, paper: str) -> layout.Page:
    if paper.upper() == "AUTO":
        # 도면틀 비율로 방향만 정하고 A3 크기 사용
        paper = "A3"
    w, h = PAPER_MM[paper.upper()]
    if frame.height > frame.width:
        w, h = h, w
    return layout.Page(w, h, layout.Units.mm, margins=layout.Margins.all(0))


def render_frame_pdf(doc, frame: Frame, out_path: Path, *, paper: str = "A3", color: bool = False,
                     cache: bbox.Cache | None = None) -> Path:
    ents = entities_in_frame(doc, frame, cache=cache)
    backend = PyMuPdfBackend()
    cfg = Configuration(
        background_policy=BackgroundPolicy.WHITE,
        color_policy=ColorPolicy.COLOR if color else ColorPolicy.BLACK,
        lineweight_policy=LineweightPolicy.ABSOLUTE,
    )
    handles = {e.dxf.handle for e in ents}
    frontend = Frontend(RenderContext(doc), backend, config=cfg)
    # draw_layout 을 써야 배경색/레이아웃 속성이 적용된다 (draw_entities 는 모델공간 검은 배경이 남음)
    frontend.draw_layout(doc.modelspace(), finalize=True, filter_func=lambda e: e.dxf.handle in handles)
    page = page_for(frame, paper)
    settings = layout.Settings(fit_page=True, crop_at_margins=True)
    pdf = backend.get_pdf_bytes(page, settings=settings, render_box=frame.box)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(pdf)
    return out_path


def safe_filename(s: str, limit: int = 80) -> str:
    s = re.sub(r'[\\/:*?"<>|\r\n\t]+', "_", s).strip(" ._")
    return s[:limit] or "untitled"


def merge_pdfs(paths: list[Path], out_path: Path) -> Path:
    import pymupdf

    merged = pymupdf.open()
    for p in paths:
        with pymupdf.open(str(p)) as src:
            merged.insert_pdf(src)
    merged.save(str(out_path))
    merged.close()
    return out_path
