"""Create the optional TF card font kit alongside the generated website."""
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo

site = Path(__file__).resolve().parents[1]
source = site.parent / "main/assets/fonts"
target = site / "dist/downloads/folio-fonts.zip"
target.parent.mkdir(parents=True, exist_ok=True)
files = [source / f"folio_{face}.ttf" for face in ("hei", "kai", "yuan", "symbols")]
files += [source / "NOTICE.txt", *sorted((source / "licenses").glob("*.txt"))]
with ZipFile(target, "w") as archive:
    for path in files:
        info = ZipInfo("folio-fonts/" + path.relative_to(source).as_posix())
        archive.writestr(info, path.read_bytes(), compress_type=ZIP_DEFLATED)
