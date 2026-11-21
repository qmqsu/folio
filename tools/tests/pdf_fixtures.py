"""Small text PDFs for the native reader integration test."""
import sys
import zlib
from pathlib import Path


def stream(data):
    data = zlib.compress(data)
    return b'<< /Filter /FlateDecode /Length %d >>\nstream\n' % len(data) + data + b'\nendstream'


def fixture(with_text):
    content = b'BT /F1 12 Tf 12 TL 40 730 Td (Folio PDF reader) Tj T* /F2 12 Tf [<0001> 0 <0002>] TJ ET\n'
    cmap = b'1 begincodespacerange\n<0000> <ffff>\nendcodespacerange\n2 beginbfchar\n<0001> <4e2d>\n<0002> <6587>\nendbfchar\n'
    objects = [
        b'<< /Type /Catalog /Pages 2 0 R >>',
        b'<< /Type /Pages /Count 1 /Kids [3 0 R] /Resources 8 0 R >>',
        b'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 600 800] /Contents 4 0 R >>',
        stream(content * 100 if with_text else b'q 1 0 0 1 0 0 cm Q'),
        b'<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>',
        b'<< /Type /Font /Subtype /Type0 /Encoding /Identity-H /ToUnicode 7 0 R >>',
        stream(cmap), b'<< /Font << /F1 5 0 R /F2 6 0 R >> >>',
    ]
    return document(objects)


def document(objects):
    out = bytearray(b'%PDF-1.4\n')
    offsets = [0]
    for i, obj in enumerate(objects, 1):
        offsets.append(len(out))
        out.extend(b'%d 0 obj\n' % i + obj + b'\nendobj\n')
    xref = len(out)
    out.extend(b'xref\n0 %d\n0000000000 65535 f \n' % len(offsets))
    for offset in offsets[1:]:
        out.extend(b'%010d 00000 n \n' % offset)
    out.extend(b'trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n' % (len(offsets), xref))
    return out


if __name__ == '__main__':
    folder = Path(sys.argv[1])
    folder.mkdir(parents=True, exist_ok=True)
    for name, with_text in [('Reflow.PDF', True), ('NoText.pdf', False)]:
        (folder / name).write_bytes(fixture(with_text))

    jpeg = (Path(__file__).parent / 'fixtures/cover/gradient-800x1200.jpg').read_bytes()
    image = b'<< /Type /XObject /Subtype /Image /Width 800 /Height 1200 /ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /DCTDecode /Length %d >>\nstream\n' % len(jpeg) + jpeg + b'\nendstream'
    (folder / 'Scan.pdf').write_bytes(document([
        b'<< /Type /Catalog /Pages 2 0 R >>',
        b'<< /Type /Pages /Count 2 /Kids [3 0 R 4 0 R] >>',
        b'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 800 1200] /Resources << /XObject << /Im 6 0 R >> >> /Contents 5 0 R >>',
        b'<< /Type /Page /Parent 2 0 R /MediaBox [0 0 800 1200] /Resources << /XObject << /Im 7 0 R >> >> /Contents 5 0 R >>',
        stream(b'q 800 0 0 1200 0 0 cm /Im Do Q'), image,
        b'<< /Type /XObject /Subtype /Form /BBox [0 0 1 1] /Resources << /XObject << /J 6 0 R >> >> /Length 5 >>\nstream\n/J Do\nendstream',
    ]))
