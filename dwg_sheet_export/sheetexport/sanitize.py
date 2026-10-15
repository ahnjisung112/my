"""변환된 DXF 에서 ezdxf 가 처리하지 못하는 객체를 정리한다."""
from __future__ import annotations

import ezdxf
from ezdxf.entities import BoundaryPathType, EdgeType, Hatch


def _bad_hatch(h: Hatch) -> bool:
    for p in h.paths:
        if p.type != BoundaryPathType.EDGE:
            continue
        for e in p.edges:
            if e.type == EdgeType.SPLINE:
                n, k = len(e.control_points), len(e.knot_values)
                if k != n + e.degree + 1:
                    return True
    return False


def sanitize(doc) -> int:
    """매듭(knot) 개수가 맞지 않는 스플라인 경계를 가진 HATCH 를 제거한다. 제거 개수를 돌려준다."""
    removed = 0
    layouts = [doc.modelspace()] + [doc.blocks.get(b.name) for b in doc.blocks]
    for lay in layouts:
        for h in list(lay.query("HATCH")):
            if _bad_hatch(h):
                lay.delete_entity(h)
                removed += 1
    return removed
