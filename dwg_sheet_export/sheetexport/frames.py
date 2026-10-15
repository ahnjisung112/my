"""모델 공간에서 도면틀(도곽)을 찾고, 각 도면틀의 도면명/도면번호를 읽는다.

도면틀 인식 규칙:
  * 블록 참조(INSERT) 중 블록 정의의 가로세로 비율이 √2 (A열 용지) 인 것
  * 닫힌 사각형 LWPOLYLINE 중 비율이 √2 인 것
  * 다른 도면틀 안에 완전히 포함된 것은 제외 (표, 내부 테이블 등)
"""
from __future__ import annotations

import difflib
import math
import re
from dataclasses import dataclass, field
from typing import Iterable, Optional

from ezdxf import bbox
from ezdxf.math import BoundingBox2d, Vec2

SQRT2 = math.sqrt(2)
DEFAULT_TITLE_LAYER = r"도곽|TITLE|도면명"
DEFAULT_NUMBER_REGEX = r"^[A-Z0-9]+(?:-[A-Z0-9]+){2,}$"
SCALE_REGEX = re.compile(r"^(?:S\s*=\s*)?1\s*[:/]\s*\d+$", re.I)


@dataclass
class TextItem:
    text: str
    layer: str
    height: float
    pos: Vec2


@dataclass
class Frame:
    index: int
    box: BoundingBox2d
    source: str
    title: Optional[str] = None
    number: Optional[str] = None
    scale: Optional[str] = None
    texts: list = field(default_factory=list)

    @property
    def width(self) -> float:
        return self.box.size.x

    @property
    def height(self) -> float:
        return self.box.size.y

    @property
    def label(self) -> str:
        parts = [f"#{self.index}"]
        if self.number:
            parts.append(self.number)
        parts.append(self.title or "(제목 없음)")
        return " ".join(parts)


def _ratio_ok(w: float, h: float, tol: float) -> bool:
    if w <= 0 or h <= 0:
        return False
    r = max(w, h) / min(w, h)
    return abs(r - SQRT2) <= tol


def _entity_box(entity, cache: bbox.Cache) -> Optional[BoundingBox2d]:
    try:
        b = bbox.extents([entity], fast=True, cache=cache)
    except Exception:
        return None
    if not b.has_data:
        return None
    return BoundingBox2d([b.extmin, b.extmax])


def _rect_of_lwpolyline(e) -> Optional[tuple[float, float, float, float]]:
    pts = [Vec2(p[:2]) for p in e.get_points()]
    if len(pts) == 5 and pts[0].isclose(pts[-1]):
        pts = pts[:4]
    if len(pts) != 4 or not (e.closed or len(pts) == 4):
        return None
    xs = sorted(p.x for p in pts)
    ys = sorted(p.y for p in pts)
    x0, x1, y0, y1 = xs[0], xs[-1], ys[0], ys[-1]
    eps = 1e-3 * max(x1 - x0, y1 - y0, 1.0)
    for p in pts:
        if not (abs(p.x - x0) < eps or abs(p.x - x1) < eps):
            return None
        if not (abs(p.y - y0) < eps or abs(p.y - y1) < eps):
            return None
    return x0, y0, x1 - x0, y1 - y0


def _contains(outer: BoundingBox2d, inner: BoundingBox2d, slack: float) -> bool:
    return (
        inner.extmin.x >= outer.extmin.x - slack
        and inner.extmin.y >= outer.extmin.y - slack
        and inner.extmax.x <= outer.extmax.x + slack
        and inner.extmax.y <= outer.extmax.y + slack
    )


def detect_frames(doc, *, min_size: float | None = None, ratio_tol: float = 0.03,
                  block_regex: str | None = None) -> list[Frame]:
    msp = doc.modelspace()
    cache = bbox.Cache()
    candidates: list[tuple[BoundingBox2d, str]] = []

    # 모델 전체 크기 (HATCH 제외) -> 최소 도면틀 크기 기본값
    if min_size is None:
        ents = [e for e in msp if e.dxftype() != "HATCH"]
        ext = bbox.extents(ents, fast=True, cache=cache)
        min_size = 0.02 * max(ext.size.x, ext.size.y) if ext.has_data else 0.0

    block_pat = re.compile(block_regex, re.I) if block_regex else None

    # 1) 블록 참조
    block_ratio_ok: dict[str, bool] = {}
    for ins in msp.query("INSERT"):
        name = ins.dxf.name
        if block_pat and not block_pat.search(name):
            continue
        if name not in block_ratio_ok:
            blk = doc.blocks.get(name)
            ok = False
            if blk is not None:
                try:
                    bext = bbox.extents([x for x in blk if x.dxftype() != "HATCH"], fast=True)
                    ok = bext.has_data and _ratio_ok(bext.size.x, bext.size.y, ratio_tol)
                except Exception:
                    ok = False
            block_ratio_ok[name] = ok
        if not block_ratio_ok[name]:
            continue
        b = _entity_box(ins, cache)
        if b and min(b.size.x, b.size.y) >= min_size and _ratio_ok(b.size.x, b.size.y, ratio_tol):
            candidates.append((b, f"block:{name}"))

    # 2) 닫힌 사각형 폴리라인
    for pl in msp.query("LWPOLYLINE"):
        r = _rect_of_lwpolyline(pl)
        if not r:
            continue
        x, y, w, h = r
        if min(w, h) >= min_size and _ratio_ok(w, h, ratio_tol):
            candidates.append((BoundingBox2d([(x, y), (x + w, y + h)]), f"rect:{pl.dxf.layer}"))

    # 3) 중복/포함 제거
    candidates.sort(key=lambda c: -(c[0].size.x * c[0].size.y))
    kept: list[tuple[BoundingBox2d, str]] = []
    for b, src in candidates:
        slack = 0.01 * max(b.size.x, b.size.y)
        dup = False
        for kb, _ in kept:
            if _contains(kb, b, slack):
                dup = True
                break
        if not dup:
            kept.append((b, src))

    # 4) 위에서 아래, 왼쪽에서 오른쪽 순으로 정렬
    if kept:
        row_h = min(b.size.y for b, _ in kept) * 0.5
        kept.sort(key=lambda c: (-round(c[0].extmax.y / row_h), c[0].extmin.x))
    frames = [Frame(i + 1, b, src) for i, (b, src) in enumerate(kept)]
    return frames


def collect_texts(doc) -> list[TextItem]:
    items: list[TextItem] = []
    for e in doc.modelspace().query("TEXT MTEXT"):
        if e.dxftype() == "TEXT":
            txt = e.dxf.text
            h = float(e.dxf.height)
        else:
            txt = e.plain_text()
            h = float(e.dxf.char_height)
        txt = " ".join(txt.split())
        if not txt:
            continue
        items.append(TextItem(txt, e.dxf.layer, h, Vec2(e.dxf.insert.x, e.dxf.insert.y)))
    return items


def parse_drawing_index(texts: Iterable[TextItem], number_regex: str = DEFAULT_NUMBER_REGEX) -> dict[str, dict]:
    """도면목록표에서 '도면번호 - 도면명 - 축척' 행을 읽어 {도면명: {number, scale}} 를 만든다."""
    pat = re.compile(number_regex)
    texts = list(texts)
    index: dict[str, dict] = {}
    for t in texts:
        if not pat.match(t.text):
            continue
        tol = max(t.height * 0.6, 1e-6)
        row = sorted(
            (o for o in texts if o is not t and abs(o.pos.y - t.pos.y) <= tol and o.pos.x > t.pos.x),
            key=lambda o: o.pos.x,
        )
        name = next((o.text for o in row if not pat.match(o.text) and not SCALE_REGEX.match(o.text)), None)
        scale = next((o.text for o in row if SCALE_REGEX.match(o.text)), None)
        if name:
            index[name] = {"number": t.text, "scale": scale}
    return index


def assign_titles(frames: list[Frame], texts: list[TextItem], *, title_layer: str = DEFAULT_TITLE_LAYER,
                  number_regex: str = DEFAULT_NUMBER_REGEX) -> None:
    layer_pat = re.compile(title_layer, re.I)
    num_pat = re.compile(number_regex)
    index = parse_drawing_index(texts, number_regex)
    for f in frames:
        inside = [t for t in texts if f.box.inside(t.pos)]
        f.texts = inside
        if not inside:
            continue
        # 도면틀 안에 도면번호가 직접 적혀 있으면 사용
        distinct = {t.text for t in inside if num_pat.match(t.text)}
        if len(distinct) == 1:  # 여러 개면 도면목록표이므로 사용하지 않음
            f.number = distinct.pop()
        pref = [t for t in inside if layer_pat.search(t.layer)]
        pool = pref or [t for t in inside if not num_pat.match(t.text) and not SCALE_REGEX.match(t.text)]
        if pool:
            f.title = max(pool, key=lambda t: (t.height, len(t.text))).text
        # 도면목록에서 번호/축척 보완
        if f.title and index:
            hit = index.get(f.title)
            if hit is None:
                m = difflib.get_close_matches(f.title, list(index), n=1, cutoff=0.85)
                hit = index[m[0]] if m else None
            if hit:
                f.number = f.number or hit["number"]
                f.scale = f.scale or hit["scale"]


def entities_in_frame(doc, frame: Frame, cache: bbox.Cache | None = None, margin_ratio: float = 0.0) -> list:
    """도면틀 영역과 겹치는 모델 공간 객체 목록."""
    cache = cache or bbox.Cache()
    m = margin_ratio * max(frame.width, frame.height)
    region = BoundingBox2d([frame.box.extmin - Vec2(m, m), frame.box.extmax + Vec2(m, m)])
    out = []
    for e in doc.modelspace():
        b = _entity_box(e, cache)
        if b is None:
            continue
        if region.has_intersection(b):
            out.append(e)
    return out
