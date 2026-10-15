# dwg_sheet_export

두 가지 형태로 제공됩니다.

| 형태 | 파일 | 특징 |
|---|---|---|
| **브라우저 도구 (HTML)** | `dist/dwg2pdf.html`, `dist/dwg2pdf_offline.html` | 설치 없이 브라우저에서 DWG를 열어 도면틀을 고르고 PDF로 저장. 파일은 PC 밖으로 나가지 않음 |
| **명령행 도구 (Python)** | `sheetexport/` | 일괄 처리·자동화용. 벡터 PDF 출력 |

## 브라우저 도구 (dwg2pdf.html)

* `dist/dwg2pdf.html` — 35 KB. 처음 열 때 DWG 엔진(LibreDWG WebAssembly, 약 11 MB)과 PDF 라이브러리를 CDN에서 받습니다. 인터넷이 필요합니다.
* `dist/dwg2pdf_offline.html` — 약 15 MB. 엔진을 파일 안에 내장해 인터넷 없이 동작합니다. `python build_html.py --offline` 으로 만듭니다 (git 에는 올리지 않음).

사용: 파일을 브라우저로 열고 → DWG 끌어다 놓기 → 도면 카드에서 선택 → **선택 도면 → PDF 저장** (여러 장이면 한 PDF로 묶임) 또는 **개별 PDF (ZIP)**. DXF 변환 저장도 됩니다.
출력은 A3 가로(기본) 200 dpi 이미지 기반 PDF이며 해상도·용지·흑백/컬러·선 굵기를 바꿀 수 있습니다.

소스는 `dwg2pdf.src.html`(본문 조각)이고 `build_html.py` 가 완전한 HTML 로 묶습니다. 브라우저 안에서 도면틀 인식 로직은 아래 Python 과 같은 규칙입니다.

## 웹사이트에 올리기 (Cloudflare Pages 등)

`python build_html.py --site` 를 실행하면 `site/dwg2pdf/` 폴더가 만들어집니다. 엔진과 PDF 라이브러리가 폴더 안에 들어 있어 외부 CDN 없이 동작합니다.

```
site/dwg2pdf/
├─ index.html
└─ vendor/   (LibreDWG WebAssembly 11 MB, jsPDF, JSZip)
```

* **Git 연동 사이트**: 이 `dwg2pdf` 폴더를 사이트 저장소의 배포 폴더(빌드 출력 폴더) 안에 그대로 복사해 커밋합니다. `https://사이트주소/dwg2pdf/` 로 열립니다.
* **직접 업로드 사이트**: Cloudflare 대시보드의 Workers & Pages → 해당 프로젝트 → Create deployment 에서 기존 사이트 파일과 함께 `dwg2pdf` 폴더를 올립니다. 직접 업로드는 매번 사이트 전체를 올려야 하므로 기존 파일도 같이 넣어야 합니다.
* **메뉴 연결**: 사이트의 메뉴나 첫 화면에 `<a href="/dwg2pdf/">DWG 도면 PDF 변환</a>` 링크를 추가합니다. 페이지 안에 넣고 싶으면 `<iframe src="/dwg2pdf/" style="width:100%;height:90vh;border:0"></iframe>` 도 됩니다.
* Cloudflare Pages 의 파일당 25 MB 제한 안에 들어갑니다 (가장 큰 파일 11 MB).

## 명령행 도구 (sheetexport)

한 시트(모델 공간)에 여러 도면이 나란히 들어 있는 DWG 파일에서 **도면틀(도곽)을 자동으로 인식**하고,
원하는 도면만 골라 **개별 PDF**로 출력하는 도구입니다.

```
도면.dwg (14개 도면이 한 시트에)  ─▶  list  ─▶  #7  0-32300-OM-101-953  1:30  제1소수력 수차실 철골계단 상세도면-1 ...
                                  ─▶  export --select 953  ─▶  07_0-32300-OM-101-953_제1소수력 수차실 철골계단 상세도면-1.pdf
```

## 동작 원리

1. **DWG → DXF 변환**
   * ODA File Converter 가 설치되어 있으면 그것을 사용합니다 (가장 정확).
   * 없으면 Node.js 위에서 LibreDWG(WebAssembly 빌드, `@mlightcad/libredwg-web`) 로 변환합니다. AutoCAD 가 필요 없습니다.
2. **도면틀 인식** (`sheetexport/frames.py`)
   * 블록 참조(INSERT) 중 블록 정의의 가로세로 비율이 √2(A열 용지)인 것
   * 닫힌 사각형 폴리라인 중 비율이 √2 인 것
   * 다른 도면틀 안에 완전히 포함된 사각형(표, 내부 테이블)은 제외
3. **도면명 / 도면번호 읽기**
   * 도면틀 안의 문자 중 `도곽|TITLE|도면명` 레이어의 가장 큰 글자를 도면명으로 사용
   * 도면틀 안에 도면번호 형식(`A-BBBB-CC-DDD-EEE` 같은 하이픈 3개 이상) 문자가 하나 있으면 도면번호로 사용
   * 도면목록표(표지)가 있으면 도면명으로 도면번호·축척을 보완
4. **PDF 출력** (`sheetexport/render.py`)
   * ezdxf 렌더러 + PyMuPDF 로 도면틀 영역만 잘라 벡터 PDF 생성 (기본 A3 가로, 흑백)
   * 시스템에 없는 글꼴(굴림, 돋움, SHX 등)은 한글 글꼴로 자동 대체 (맑은 고딕 → 나눔고딕 → Noto 순)

## 설치

```bash
# Python 3.10+
pip install -r requirements.txt

# Node.js 20+ (DWG 변환용, ODA File Converter 가 있으면 생략 가능)
npm install        # dwg_sheet_export 폴더에서. 처음 실행 때 자동으로도 설치됩니다.
```

Windows 에서는 맑은 고딕(`malgun.ttf`)이 기본으로 있어 한글 글꼴을 따로 설치할 필요가 없습니다.
Linux 는 `sudo apt install fonts-nanum` 을 권장합니다.

## 사용법

`dwg_sheet_export` 폴더에서 실행합니다.

```bash
# 1) 시트 안의 도면 목록 보기
python -m sheetexport list 도면.dwg

# 2) 원하는 도면만 PDF 로
python -m sheetexport export 도면.dwg --out output --select 953            # 도면번호 끝자리
python -m sheetexport export 도면.dwg --out output --select 7 11           # 목록의 # 번호
python -m sheetexport export 도면.dwg --out output --select 사다리         # 도면명 일부
python -m sheetexport export 도면.dwg --out output --select 953,954,사다리  # 섞어서

# 3) 전부 출력하고 한 파일로 합치기
python -m sheetexport export 도면.dwg --out output --all --merge 전체도면.pdf

# 4) DXF 변환만
python -m sheetexport convert 도면.dwg 도면.dxf
```

주요 옵션

| 옵션 | 설명 |
|---|---|
| `--paper A3` | 출력 용지 (A0~A4). 도면틀 방향에 맞춰 가로/세로 자동 |
| `--color` | 원본 색상 유지 (기본은 흑백 출력) |
| `--font malgun.ttf` | 한글 대체 글꼴 파일명 지정 |
| `--title-layer "도곽\|TITLE"` | 도면명이 들어 있는 레이어 (정규식) |
| `--number-regex` | 도면번호 형식 (정규식) |
| `--block-regex "^box"` | 도면틀 블록 이름을 알고 있을 때 직접 지정 |
| `--min-frame 5000` | 도면틀 최소 크기 (도면 단위). 기본값은 전체 크기의 2% |
| `--method node` | DWG 변환 방식 강제 (`auto`/`oda`/`node`) |

출력 파일명은 `순번_도면번호_도면명.pdf` 형식입니다.

## 검증한 파일

당진 제1,2소수력 편의시설 설치공사 도면(AutoCAD 2018 형식, 4.6 MB)으로 확인했습니다.
도면틀 14개(표지 2 + 도면 12)를 모두 인식했고, 도면번호·도면명·축척을 표제란과 도면목록표에서 읽어 냈습니다.

## 알려진 제약

* 선 굵기는 객체/레이어의 선가중치를 따릅니다. 색상별 플롯 스타일(CTB)은 아직 반영하지 않습니다.
* 도면이 배치(레이아웃) 탭에 뷰포트로 구성된 파일은 지원하지 않습니다 (모델 공간 기준).
* LibreDWG 가 읽지 못하는 손상된 HATCH 는 건너뜁니다 (실행 시 개수를 출력).
* 도면 한 장당 수십 초가 걸릴 수 있습니다 (한글 글꼴 벡터화 때문).
