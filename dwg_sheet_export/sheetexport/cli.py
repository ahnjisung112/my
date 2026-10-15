"""명령행 인터페이스.

  python -m sheetexport list   도면.dwg
  python -m sheetexport export 도면.dwg --out output --select 953,사다리
  python -m sheetexport export 도면.dwg --out output --all --merge 전체.pdf
  python -m sheetexport convert 도면.dwg 도면.dxf
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from ezdxf import bbox

from . import __version__
from .convert import dwg_to_dxf, load_document
from .frames import DEFAULT_NUMBER_REGEX, DEFAULT_TITLE_LAYER, Frame, assign_titles, collect_texts, detect_frames
from .render import merge_pdfs, pick_fallback_font, render_frame_pdf, safe_filename, substitute_missing_fonts
from .sanitize import sanitize


def _common(p: argparse.ArgumentParser) -> None:
    p.add_argument("file", help="입력 DWG 또는 DXF")
    p.add_argument("--workdir", help="DWG->DXF 변환 결과를 둘 폴더 (기본: 입력 파일 폴더)")
    p.add_argument("--method", choices=["auto", "oda", "node"], default="auto", help="DWG 변환 방식")
    p.add_argument("--title-layer", default=DEFAULT_TITLE_LAYER, help="도면명 텍스트가 있는 레이어 (정규식)")
    p.add_argument("--number-regex", default=DEFAULT_NUMBER_REGEX, help="도면번호 형식 (정규식)")
    p.add_argument("--block-regex", help="도면틀 블록 이름 필터 (정규식). 생략하면 비율로 자동 판별")
    p.add_argument("--min-frame", type=float, help="도면틀 최소 크기 (도면 단위). 기본: 전체 크기의 2%%")
    p.add_argument("--ratio-tol", type=float, default=0.03, help="가로세로 비율 √2 허용 오차")


def _load(args) -> tuple[object, list[Frame]]:
    doc = load_document(args.file, workdir=args.workdir, method=args.method)
    removed = sanitize(doc)
    if removed:
        print(f"[sanitize] 손상된 HATCH {removed}개 제거")
    frames = detect_frames(doc, min_size=args.min_frame, ratio_tol=args.ratio_tol, block_regex=args.block_regex)
    assign_titles(frames, collect_texts(doc), title_layer=args.title_layer, number_regex=args.number_regex)
    return doc, frames


def _print_frames(frames: list[Frame]) -> None:
    if not frames:
        print("도면틀을 찾지 못했습니다. --block-regex 또는 --min-frame 옵션을 확인하세요.")
        return
    print(f"도면틀 {len(frames)}개")
    for f in frames:
        num = f.number or "-"
        scale = f.scale or "-"
        print(f"  #{f.index:<3} {num:<22} {scale:<6} {f.title or '(제목 없음)'}   "
              f"[{f.box.extmin.x:.0f},{f.box.extmin.y:.0f} ~ {f.box.extmax.x:.0f},{f.box.extmax.y:.0f}] {f.source}")


def _matches(f: Frame, token: str) -> bool:
    token = token.strip()
    if not token:
        return False
    if token.isdigit() and int(token) == f.index:
        return True
    if f.number and (f.number == token or f.number.endswith("-" + token) or f.number.endswith(token)):
        return True
    if f.title and token.lower() in f.title.lower():
        return True
    return False


def cmd_list(args) -> int:
    _, frames = _load(args)
    if args.json:
        print(json.dumps([{
            "index": f.index, "number": f.number, "title": f.title, "scale": f.scale,
            "bbox": [f.box.extmin.x, f.box.extmin.y, f.box.extmax.x, f.box.extmax.y], "source": f.source,
        } for f in frames], ensure_ascii=False, indent=2))
    else:
        _print_frames(frames)
    return 0


def cmd_export(args) -> int:
    doc, frames = _load(args)
    _print_frames(frames)
    if not frames:
        return 1
    if args.all:
        selected = frames
    else:
        tokens = [t for part in args.select for t in part.split(",")]
        selected = [f for f in frames if any(_matches(f, t) for t in tokens)]
        missing = [t for t in tokens if not any(_matches(f, t) for f in frames)]
        for t in missing:
            print(f"[warn] '{t}' 에 해당하는 도면이 없습니다.")
    if not selected:
        print("선택된 도면이 없습니다.")
        return 1

    font = pick_fallback_font(args.font)
    if font:
        changed = substitute_missing_fonts(doc, font)
        if changed:
            print(f"[font] 시스템에 없는 글꼴 {len(changed)}개 스타일을 '{font}' 로 대체")
    else:
        print("[font] 한글 글꼴을 찾지 못했습니다. --font 로 TTF 파일명을 지정하세요.")

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    cache = bbox.Cache()
    written: list[Path] = []
    for f in selected:
        parts = [f"{f.index:02d}"]
        if f.number:
            parts.append(f.number)
        parts.append(f.title or "untitled")
        name = safe_filename("_".join(parts)) + ".pdf"
        path = out_dir / name
        print(f"[export] {f.label} -> {path}")
        render_frame_pdf(doc, f, path, paper=args.paper, color=args.color, cache=cache)
        written.append(path)
    if args.merge:
        merged = out_dir / args.merge if not Path(args.merge).is_absolute() else Path(args.merge)
        merge_pdfs(written, merged)
        print(f"[merge] {len(written)}장 -> {merged}")
    print(f"완료: PDF {len(written)}개")
    return 0


def cmd_convert(args) -> int:
    out = dwg_to_dxf(args.file, args.output, method=args.method)
    print(f"DXF 저장: {out}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="sheetexport", description="DWG 한 시트 안의 도면들을 개별 PDF 로 출력")
    p.add_argument("--version", action="version", version=__version__)
    sub = p.add_subparsers(dest="cmd", required=True)

    pl = sub.add_parser("list", help="도면틀 목록 보기")
    _common(pl)
    pl.add_argument("--json", action="store_true")
    pl.set_defaults(func=cmd_list)

    pe = sub.add_parser("export", help="선택한 도면을 PDF 로 출력")
    _common(pe)
    pe.add_argument("--out", default="output", help="PDF 출력 폴더")
    g = pe.add_mutually_exclusive_group(required=True)
    g.add_argument("--select", nargs="+", default=[], help="번호(#), 도면번호(끝자리 가능), 도면명 일부. 쉼표/공백 구분")
    g.add_argument("--all", action="store_true", help="모든 도면 출력")
    pe.add_argument("--paper", default="A3", choices=["A0", "A1", "A2", "A3", "A4", "auto"], help="출력 용지 (기본 A3)")
    pe.add_argument("--color", action="store_true", help="원본 색상 유지 (기본: 흑백)")
    pe.add_argument("--font", help="한글 대체 글꼴 파일명 (예: malgun.ttf)")
    pe.add_argument("--merge", help="선택한 도면을 한 PDF 로 합쳐 저장할 파일명")
    pe.set_defaults(func=cmd_export)

    pc = sub.add_parser("convert", help="DWG -> DXF 변환만 수행")
    pc.add_argument("file")
    pc.add_argument("output")
    pc.add_argument("--method", choices=["auto", "oda", "node"], default="auto")
    pc.set_defaults(func=cmd_convert)
    return p


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return args.func(args)
    except Exception as e:  # noqa: BLE001
        print(f"오류: {e}", file=sys.stderr)
        return 1
