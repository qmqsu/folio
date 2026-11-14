#!/usr/bin/env python3
"""Build Folio's OFL system font: GB2312 level-1, ASCII and all native UI characters.
Requires fonttools. Input is the upstream NotoSerifSC variable TrueType file.
"""
import argparse,hashlib,json,re,struct,zlib
from pathlib import Path
from fontTools.ttLib import TTFont
from fontTools.varLib.instancer import instantiateVariableFont
from fontTools import subset
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('source',type=Path);args=p.parse_args()
font=TTFont(args.source);instantiateVariableFont(font,{'wght':500},inplace=True)
chars=set(range(32,127))
for first in range(0xa1,0xd8):
 for second in range(0xa1,0xff):
  try:chars.add(ord(bytes([first,second]).decode('gb2312')))
  except UnicodeDecodeError:pass
for file in (ROOT/'main').rglob('*'):
 if file.suffix in ('.c','.h','.inc','.cpp') and 'vendor' not in file.parts:
  chars.update(ord(c) for c in file.read_text() if 0x3000<=ord(c)<=0xffef)
chars.update([0x2013,0x2014,0x2018,0x2019,0x201c,0x201d,0x2026,0xb7])
options=subset.Options();options.hinting=False;options.layout_features=[]
worker=subset.Subsetter(options=options);worker.populate(unicodes=chars);worker.subset(font)
for table in ('GPOS','GSUB','GDEF','BASE','DSIG'):
 if table in font:del font[table]
# Modified font uses its own family name, retaining copyright/license records.
for name_id,text in [(1,'Folio Songti'),(2,'Medium'),(3,'FolioSongti-Medium-2'),(4,'Folio Songti Medium'),(6,'FolioSongti-Medium')]:
 font['name'].setName(text,name_id,3,1,0x409)
out=ROOT/'main/assets';out.mkdir(exist_ok=True);target=out/'folio_builtin.ttf';font.save(target)
raw=target.read_bytes();co=zlib.compressobj(9,zlib.DEFLATED,-15);packed=co.compress(raw)+co.flush()
(out/'folio_builtin.deflate').write_bytes(packed)
(out/'folio_builtin.h').write_text('#pragma once\n#define FOLIO_BUILTIN_FONT_BYTES '+str(len(raw))+'u\n')
report={'upstream':'https://github.com/google/fonts/tree/main/ofl/notoserifsc','upstream_sha256':hashlib.sha256(args.source.read_bytes()).hexdigest(),'weight':500,'coverage':'GB2312 level-1 + ASCII + native UI characters','glyphs':len(font.getBestCmap()),'ttf_bytes':len(raw),'deflate_bytes':len(packed),'ttf_sha256':hashlib.sha256(raw).hexdigest(),'deflate_sha256':hashlib.sha256(packed).hexdigest(),'license':'SIL OFL 1.1; see licenses/NotoSerifSC-OFL.txt'}
(out/'folio_builtin.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n');print(json.dumps(report,indent=2))
