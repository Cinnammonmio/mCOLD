"""Tessellate the case STEP into one STL per part for the mockup renderer.

usage: python mockup/prepare.py "path/to/Assy CASE_80CM_V2x.STEP"
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

# e-paper texture: the A2 TRIP ACTIVE screen at 8x, inks mapped to panel tones
from PIL import Image
a2 = Image.open(os.path.join(here, '..', 'display-mock', 'out', 'A2_trip.png')).convert('L')
a2 = a2.resize((a2.width * 8, a2.height * 8), Image.NEAREST).point(lambda v: 255 if v > 128 else 0)
os.makedirs(os.path.join(here, 'build'), exist_ok=True)
Image.composite(Image.new('RGB', a2.size, (234, 233, 226)), Image.new('RGB', a2.size, (28, 29, 31)), a2).save(
    os.path.join(here, 'build', 'screen_tex.png'))
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
