"""DWG -> DXF 변환.

우선순위:
1. ODA File Converter 가 설치되어 있으면 그것을 사용 (가장 정확).
2. 없으면 Node.js + LibreDWG(WebAssembly) 로 변환 (dwg2dxf.mjs).
"""
from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path

PKG_DIR = Path(__file__).resolve().parent.parent  # dwg_sheet_export/
NODE_SCRIPT = PKG_DIR / "dwg2dxf.mjs"


class ConversionError(RuntimeError):
    pass


def oda_available() -> bool:
    try:
        from ezdxf.addons import odafc
        return odafc.is_installed()
    except Exception:
        return False


def convert_with_oda(src: Path, dst: Path) -> None:
    from ezdxf.addons import odafc
    odafc.convert(str(src), str(dst), version="R2018", replace=True)


def _ensure_node_modules() -> None:
    if (PKG_DIR / "node_modules" / "@mlightcad" / "libredwg-web").exists():
        return
    npm = shutil.which("npm")
    if npm is None:
        raise ConversionError("npm 을 찾을 수 없습니다. Node.js 20 이상을 설치하세요.")
    print("[convert] LibreDWG WebAssembly 패키지를 설치합니다 (최초 1회)...")
    subprocess.run([npm, "install", "--no-audit", "--no-fund"], cwd=PKG_DIR, check=True)


def convert_with_node(src: Path, dst: Path) -> None:
    node = shutil.which("node")
    if node is None:
        raise ConversionError("node 를 찾을 수 없습니다. Node.js 20 이상을 설치하세요.")
    _ensure_node_modules()
    proc = subprocess.run(
        [node, str(NODE_SCRIPT), str(src), str(dst)],
        cwd=PKG_DIR,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    if proc.returncode != 0 or not dst.exists():
        tail = "\n".join(proc.stderr.strip().splitlines()[-5:])
        raise ConversionError(f"LibreDWG 변환 실패 (exit {proc.returncode}):\n{tail}")


def dwg_to_dxf(src: os.PathLike | str, dst: os.PathLike | str, method: str = "auto") -> Path:
    """DWG 를 DXF 로 변환해 dst 경로를 돌려준다. method: auto | oda | node"""
    src, dst = Path(src), Path(dst)
    if not src.exists():
        raise FileNotFoundError(src)
    dst.parent.mkdir(parents=True, exist_ok=True)
    if method == "oda" or (method == "auto" and oda_available()):
        print("[convert] ODA File Converter 로 변환")
        convert_with_oda(src, dst)
    elif method in ("auto", "node"):
        print("[convert] LibreDWG(WebAssembly) 로 변환")
        convert_with_node(src, dst)
    else:
        raise ValueError(f"unknown method: {method}")
    return dst


def load_document(path: os.PathLike | str, workdir: os.PathLike | str | None = None, method: str = "auto"):
    """DWG 또는 DXF 경로를 받아 ezdxf Drawing 을 돌려준다. DWG 는 workdir 에 DXF 로 변환한다."""
    import ezdxf

    path = Path(path)
    if path.suffix.lower() == ".dwg":
        workdir = Path(workdir) if workdir else path.parent
        dxf = workdir / (path.stem + ".dxf")
        if not dxf.exists() or dxf.stat().st_mtime < path.stat().st_mtime:
            dwg_to_dxf(path, dxf, method=method)
        else:
            print(f"[convert] 기존 DXF 재사용: {dxf}")
        path = dxf
    doc = ezdxf.readfile(str(path))
    return doc
