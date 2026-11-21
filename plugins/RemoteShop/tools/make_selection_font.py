"""Build two original geometric selection glyphs; no external font assets."""
from pathlib import Path
from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen

def glyph(checked=False):
    pen = TTGlyphPen(None)
    for points in (
        [(80,80),(80,840),(840,840),(840,80)],
        [(170,170),(750,170),(750,750),(170,750)],
    ):
        pen.moveTo(points[0])
        for point in points[1:]: pen.lineTo(point)
        pen.closePath()
    if checked:
        pen.moveTo((220,480))
        for point in [(370,300),(430,300),(730,650),(650,720),(400,425),(285,550)]: pen.lineTo(point)
        pen.closePath()
    return pen.glyph()

font = FontBuilder(1000, isTTF=True)
font.setupGlyphOrder([".notdef", "square", "checked"])
font.setupCharacterMap({0xE000:"square", 0xE001:"checked"})
font.setupGlyf({".notdef":TTGlyphPen(None).glyph(), "square":glyph(), "checked":glyph(True)})
font.setupHorizontalMetrics({name:(920,0) for name in [".notdef","square","checked"]})
font.setupHorizontalHeader(ascent=900, descent=-100)
font.setupNameTable({"familyName":"Remote Shop Selection", "styleName":"Regular",
    "uniqueFontIdentifier":"RemoteShopSelection-1", "fullName":"Remote Shop Selection",
    "psName":"RemoteShopSelection", "version":"Version 1.0"})
font.setupOS2(sTypoAscender=900,sTypoDescender=-100,usWinAscent=900,usWinDescent=100)
font.setupPost()
font.setupMaxp()
output = Path(__file__).resolve().parents[1] / "assets" / "Selection.ttf"
output.parent.mkdir(parents=True,exist_ok=True)
font.save(output)
print(output)
