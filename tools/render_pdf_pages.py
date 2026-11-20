"""Render specific PDF pages to PNG for visual inspection.

Usage:
  python tools/render_pdf_pages.py <pdf> <out_dir> <page> [page ...]
Pages are 1-based.
"""

from pathlib import Path
import sys

import pymupdf


def main() -> int:
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    pdf_path = Path(sys.argv[1])
    out_dir = Path(sys.argv[2])
    out_dir.mkdir(parents=True, exist_ok=True)
    doc = pymupdf.open(pdf_path)
    try:
        for raw in sys.argv[3:]:
            page_no = int(raw)
            page = doc[page_no - 1]
            pix = page.get_pixmap(matrix=pymupdf.Matrix(2.2, 2.2), alpha=False)
            target = out_dir / f"{pdf_path.stem}_p{page_no:03d}.png"
            pix.save(target)
            print(target)
    finally:
        doc.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
