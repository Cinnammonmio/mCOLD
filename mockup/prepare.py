"""Tessellate the case STEP into one STL per part for the mockup renderer.

usage: python mockup/prepare.py "path/to/Assy CASE_80CM_V2x.STEP"
       python mockup/prepare.py --screens      (only the e-paper textures)
"""
import sys, os, numpy as np
from OCP.STEPCAFControl import STEPCAFControl_Reader
from OCP.TDocStd import TDocStd_Document
from OCP.TCollection import TCollection_ExtendedString
from OCP.XCAFDoc import XCAFDoc_DocumentTool
from OCP.TDF import TDF_Label
from OCP.collections import Sequence_TDF_Label as TDF_LabelSequence
from OCP.TDataStd import TDataStd_Name
from OCP.BRepMesh import BRepMesh_IncrementalMesh
from OCP.TopExp import TopExp_Explorer
from OCP.TopAbs import TopAbs_FACE, TopAbs_SHELL, TopAbs_REVERSED
from OCP.BRep import BRep_Tool
from OCP.TopLoc import TopLoc_Location
from OCP.TopoDS import TopoDS
import trimesh

src = sys.argv[1]
here = os.path.dirname(os.path.abspath(__file__))
out = os.path.join(here, 'build', 'stl')

# e-paper textures: screens drawn by display-mock with the TCOLD device ID, at 8x,
# each ink mapped to its panel tone. screen_tex.png is A2 TRIP ACTIVE; the others
# (screen_<code>.png) show the device in other states on the FUNCTION slides.
from PIL import Image
sys.path.insert(0, os.path.join(here, '..', 'display-mock'))
import render as epd
os.makedirs(os.path.join(here, 'build'), exist_ok=True)
epd.OUT = __import__('pathlib').Path(here) / 'build'
ID = dict(device='TCOLD-0117')
SCREENS = {
    'A1': lambda: epd.ready('epd_A1', battery=78, storage=2, **ID),
    'A2': lambda: epd.monitor('epd_A2', **ID, **epd.LIVE),
    'A3': lambda: epd.monitor('epd_A3', temp='8.4', red=True, minmax=('2.8', '8.4'), **ID, **epd.LIVE),
    'A4': lambda: epd.monitor('epd_A4', temp='11.6', alarm=True, minmax=('2.8', '11.6'), trip=True, shock=True,
                              wifi=True, cloud=False, gnss=True, battery=61, storage=7, **ID),
    'C1': lambda: epd.detail('epd_C1', title='TRIP 0142', state='CLOSED', rows=(
        ('Duration', '3d 06h 12m'), ('Temperature', '2.8 / 6.1 ~C'), ('Alarms', '1 high|2 shock')),
        wifi=True, cloud=True, gnss=True, battery=74, storage=9, **ID),
}
PANEL = {epd.WHITE: (234, 233, 226), epd.BLACK: (28, 29, 31), epd.RED: (184, 50, 46)}
def texture(code):
    SCREENS[code]()
    im = Image.open(os.path.join(here, 'build', f'epd_{code}.png')).convert('RGB')
    im = im.resize((im.width * 8, im.height * 8), Image.NEAREST)
    pal = list(PANEL)
    near = {}
    def tone(p):
        if p not in near:
            near[p] = PANEL[min(pal, key=lambda q: sum((a - b) ** 2 for a, b in zip(p, q)))]
        return near[p]
    out = Image.new('RGB', im.size); out.putdata([tone(p) for p in im.getdata()])
    return out
for code in SCREENS:
    t = texture(code)
    t.save(os.path.join(here, 'build', f'screen_{code}.png'))
    if code == 'A2':
        t.save(os.path.join(here, 'build', 'screen_tex.png'))
if len(sys.argv) > 1 and sys.argv[1] == '--screens':
    sys.exit(0)
os.makedirs(out, exist_ok=True)
doc = TDocStd_Document(TCollection_ExtendedString("doc"))
r = STEPCAFControl_Reader(); r.SetNameMode(True); r.SetColorMode(True)
r.ReadFile(src); r.Transfer(doc)
st = XCAFDoc_DocumentTool.ShapeTool_s(doc.Main())
labels = TDF_LabelSequence(); st.GetFreeShapes(labels)

def name(l):
    a = TDataStd_Name()
    if l.FindAttribute(TDataStd_Name.GetID_s(), a): return a.Get().ToExtString()
    return "?"

parts = []
def walk(l, loc):
    if st.IsAssembly_s(l):
        comps = TDF_LabelSequence(); st.GetComponents_s(l, comps)
        for i in range(1, comps.Length()+1):
            c = comps.Value(i); ref = TDF_Label(); st.GetReferredShape_s(c, ref)
            walk(ref, loc * st.GetLocation_s(c))
    else:
        parts.append((name(l), st.GetShape_s(l).Moved(loc)))
for i in range(1, labels.Length()+1):
    walk(labels.Value(i), TopLoc_Location())

def mesh(shp):
    """Triangles of every face in shp (already meshed) as one trimesh."""
    V, Fs, off = [], [], 0
    ex = TopExp_Explorer(shp, TopAbs_FACE)
    while ex.More():
        f = TopoDS.Face(ex.Current()); lc = TopLoc_Location()
        t = BRep_Tool.Triangulation_s(f, lc)
        if t:
            tr = lc.Transformation()
            for k in range(1, t.NbNodes()+1):
                p = t.Node(k).Transformed(tr); V.append((p.X(), p.Y(), p.Z()))
            rev = f.Orientation() == TopAbs_REVERSED
            for k in range(1, t.NbTriangles()+1):
                a, b, c = t.Triangle(k).Get()
                Fs.append((a+off-1, c+off-1, b+off-1) if rev else (a+off-1, b+off-1, c+off-1))
            off += t.NbNodes()
        ex.Next()
    return trimesh.Trimesh(np.array(V).reshape(-1, 3), np.array(Fs, dtype=int).reshape(-1, 3), process=True)

def pcb_kind(lo, hi):
    """The PCB STEP carries no colours, so sort its 2148 shells by size for the explode render."""
    d = hi - lo; area = d[0] * d[1]
    if area > 5000: return 'board'
    if 8.5 < lo[0] + 0.3 < 9.6 and -26 < lo[1] < -25: return 'black'      # buzzer MLT-8530
    if area >= 60: return 'metal'                                       # ESP32 shield, GNSS, microSD, USB-C
    if d[2] < 0.3: return 'tin'                                         # pins and pads
    if area < 6 and d[2] < 1.0: return 'tan'                            # chip passives
    return 'black'

for idx,(nm, shp) in enumerate(parts):
    BRepMesh_IncrementalMesh(shp, 0.05, False, 0.3, True)
    m = mesh(shp)
    fn = "".join(ch if ch.isalnum() else "_" for ch in nm)[:40]+f"_{idx}"
    m.export(f"{out}/{fn}.stl")
    print(f"{nm!r:60} faces={len(m.faces):7} min={np.round(m.bounds[0],1)} max={np.round(m.bounds[1],1)} -> {fn}.stl")
    if nm.startswith('3D_PCB_foam'):
        groups = {}
        ex = TopExp_Explorer(shp, TopAbs_SHELL)
        while ex.More():
            sm = mesh(ex.Current())
            if len(sm.faces):
                groups.setdefault(pcb_kind(*sm.bounds), []).append(sm)
            ex.Next()
        for k, ms in groups.items():
            trimesh.util.concatenate(ms).export(f"{out}/pcb_{k}.stl")
            print(f"  pcb_{k}.stl  shells={len(ms)}")
