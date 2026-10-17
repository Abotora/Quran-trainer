"""Build offline Arabic glyph/word data using HarfBuzz and FreeType.
pip install uharfbuzz freetype-py pillow
Sources and licenses are recorded in README.md. Quran UTF-8 text is unchanged.
"""
import json, hashlib, math
from pathlib import Path
import uharfbuzz as hb
import freetype
from PIL import Image

ROOT=Path(__file__).resolve().parent
FONT_SIZE=24
quran=json.loads((ROOT/'quran.json').read_text())
assert len(quran)==114 and sum(len(s['verses']) for s in quran)==6236
font_bytes=(ROOT/'NotoNaskhArabic.ttf').read_bytes()
font=hb.Font(hb.Face(font_bytes)); font.scale=(FONT_SIZE*64,FONT_SIZE*64)
hb.ot_font_set_funcs(font)
face=freetype.Face(str(ROOT/'NotoNaskhArabic.ttf'))
face.set_pixel_sizes(0,FONT_SIZE)
round_px=lambda v: int(math.floor(v/64+0.5))
glyph_map={}; glyphs=[]; bitmap=bytearray()
words=[]; word_map={}; runs=[]

def glyph(gid):
    if gid in glyph_map: return glyph_map[gid]
    assert gid != 0, 'Missing font glyph'
    face.load_glyph(gid, freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_NORMAL)
    b=face.glyph.bitmap
    w,h=b.width,b.rows
    data=bytes(b.buffer); stride=(w+7)//8
    start=len(bitmap)
    for y in range(h):
        for byte in range(stride):
            n=0
            for bit in range(8):
                x=byte*8+bit
                if x<w and data[y*b.pitch+x]>=80: n |= 1 << (7-bit)
            bitmap.append(n)
    index=len(glyphs); glyph_map[gid]=index
    glyphs.append((start,w,h,face.glyph.bitmap_left,-face.glyph.bitmap_top))
    return index

def word(text):
    if text in word_map: return word_map[text]
    buf=hb.Buffer(); buf.add_str(text); buf.guess_segment_properties(); hb.shape(font,buf)
    start=len(runs); advance=0; left=0; right=0
    for info,pos in zip(buf.glyph_infos,buf.glyph_positions):
        gi=glyph(info.codepoint)
        gx=round_px(advance+pos.x_offset); gy=-round_px(pos.y_offset)
        _,w,h,bx,by=glyphs[gi]
        left=min(left,gx+bx); right=max(right,gx+bx+w)
        assert -128<=gx<=127 and -128<=gy<=127
        runs.append((gi,gx,gy))
        advance+=pos.x_advance
    width=max(right,round_px(advance))-left
    # Store a left bearing correction so marks at edges never get clipped.
    index=len(words); word_map[text]=index
    assert len(runs)-start<256 and width<256 and -left<128
    words.append((start,len(runs)-start,width,-left))
    return index

sequence=[]; ayahs=[]; surahs=[]; name_words=[]
for s in quran:
    assert s['id']==len(surahs)+1 and len(s['verses'])==s['total_verses']
    ns=len(name_words); name_words.extend(word(t) for t in s['name'].split())
    first=len(ayahs)
    for v in s['verses']:
        assert v['id']==len(ayahs)-first+1
        ws=[word(t) for t in v['text'].split()]
        start=len(sequence); sequence.extend(ws); ayahs.append((start,len(ws)))
    surahs.append((first,len(s['verses']),ns,len(name_words)-ns))

LABELS=['السور','استكمال','السابق','التالي','إخفاء','إظهار','رجوع','الآية مخفية','عرض الآية','تكملة الآية','القرآن الكريم','حفظ ومراجعة','ابدأ','الكلمة التالية','كشف الآية','إعادة','القائمة','تم الكشف','جزء الآية','الإعدادات','السطوع','مهلة الشاشة','لا تنطفئ أبدا','المظهر','ليلي','ساطع','حجم الخط','صغير','متوسط','كبير','بدء الصفحة مخفية','نعم','لا','عند التشغيل','معايرة اللمس','ضبط الإعدادات','مسح تقدم الحفظ','معلومات الجهاز','تأكيد','إلغاء','مسح تقدم الحفظ؟','ضبط الإعدادات؟','جهاز تحفيظ القرآن الكريم','بواسطة أبو تورا','الإصدار 1.0','يلزم توصيل الإضاءة','ثانية','معطلة']
label_seq=[]; labels=[]
for text in LABELS:
    start=len(label_seq); label_seq.extend(word(t) for t in text.split()); labels.append((start,len(label_seq)-start))

def emit_numbers(f,name,ctype,values):
    f.write(f'const {ctype} {name}[] PROGMEM = {{\n')
    for i in range(0,len(values),24): f.write(','.join(str(v) for v in values[i:i+24])+',\n')
    f.write('};\n')
def emit_struct(f,name,ctype,values):
    f.write(f'const {ctype} {name}[] PROGMEM = {{\n')
    for row in values: f.write('{'+','.join(map(str,row))+'},\n')
    f.write('};\n')

with (ROOT/'quran_data.h').open('w') as f:
    f.write('// Generated Arabic glyphs and Quran word sequences. See README for sources.\n#pragma once\n#include <Arduino.h>\n')
    f.write('struct QGlyph { uint32_t offset; uint8_t w,h; int8_t x,y; };\n')
    f.write('struct QRun { uint8_t glyph; int8_t x,y; } __attribute__((packed));\n')
    f.write('struct QWord { uint32_t first; uint8_t count,width; int8_t bearing; } __attribute__((packed));\n')
    f.write('struct QAyah { uint32_t first; uint16_t count; };\n')
    f.write('struct QSurah { uint16_t first,count,nameFirst; uint8_t nameCount; };\n')
    f.write('struct QLabel { uint16_t first; uint8_t count; };\n')
    emit_numbers(f,'qBitmap','uint8_t',list(bitmap))
    emit_struct(f,'qGlyphs','QGlyph',glyphs)
    emit_struct(f,'qRuns','QRun',runs)
    emit_struct(f,'qWords','QWord',words)
    emit_numbers(f,'qSequence','uint16_t',sequence)
    emit_struct(f,'qAyahs','QAyah',ayahs)
    emit_struct(f,'qSurahs','QSurah',surahs)
    emit_numbers(f,'qNameWords','uint16_t',name_words)
    emit_numbers(f,'qLabelWords','uint16_t',label_seq)
    emit_struct(f,'qLabels','QLabel',labels)
    f.write(f'constexpr uint16_t Q_SURAH_COUNT=114, Q_AYAH_COUNT={len(ayahs)};\n')

assert len(glyphs) <= 256
assert len(words) <= 65536

# Preview the same shaped glyph runs that the MCU draws, not a different renderer.
def render_words(ids):
    im=Image.new('RGB',(320,240),(12,24,35)); x=305; baseline=75
    for wid in ids:
        start,count,width,bearing=words[wid]
        if x-width<14: x=305; baseline+=36
        x-=width
        for gi,gx,gy in runs[start:start+count]:
            off,w,h,bx,by=glyphs[gi]; stride=(w+7)//8
            for yy in range(h):
                for xx in range(w):
                    if bitmap[off+yy*stride+xx//8] & (128>>(xx%8)):
                        px=x+bearing+gx+bx+xx; py=baseline+gy+by+yy
                        if 0<=px<320 and 0<=py<240: im.putpixel((px,py),(255,255,245))
        x-=6
    return im
start,count=ayahs[6]
render_words(sequence[start:start+count]).save(ROOT/'preview.png')
print('Built',len(surahs),'surahs',len(ayahs),'ayahs',len(words),'words',len(glyphs),'glyphs')
print('Approx flash bytes',len(bitmap)+len(runs)*3+len(words)*7+len(sequence)*2+len(ayahs)*8)
print('Source SHA256',hashlib.sha256((ROOT/'quran.json').read_bytes()).hexdigest())
