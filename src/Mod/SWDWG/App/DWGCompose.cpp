//! FreeCAD objects -> DWG entity records - implementation.
//!
//! 导出功能：本文件为 DWG 导出新增。详见 DWGCompose.h 的映射表。

#include "DWGCompose.h"

#include "DWGDimension.h"
#include "DWGLineGroup.h"
#include "DWGLineType.h"
#include "DWGSurfaceMesh.h"
#include "DWGText.h"

#include <App/Annotation.h>
#include <App/DocumentObject.h>
#include <App/GeoFeature.h>
#include <App/PropertyGeo.h>
#include <App/PropertyStandard.h>
#include <Base/Console.h>
#include <Mod/Part/App/PartFeature.h>
#include <Mod/Part/App/PropertyTopoShape.h>

#include <set>
#include <cstdlib>
#include <BRepAdaptor_Curve.hxx>
#include <ShapeFix_Shape.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <TopoDS_Iterator.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <TopLoc_Location.hxx>
#include <Poly_Triangulation.hxx>
#include <BRep_Tool.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <GeomAbs_CurveType.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Circ.hxx>
#include <gp_Elips.hxx>
#include <gp_Pnt.hxx>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <map>

namespace SWDWG
{
namespace
{



constexpr double kPi = 3.14159265358979323846;

//! 导出功能：Base::Color -> 0xRRGGBB（DWG 真彩色 / ACI 查找用）。
unsigned long packedRgb(const Base::Color& color)
{
    const int r = (int)std::lround(std::min(std::max(color.r, 0.0f), 1.0f) * 255.0f);
    const int g = (int)std::lround(std::min(std::max(color.g, 0.0f), 1.0f) * 255.0f);
    const int b = (int)std::lround(std::min(std::max(color.b, 0.0f), 1.0f) * 255.0f);
    return ((unsigned long)r << 16) | ((unsigned long)g << 8) | (unsigned long)b;
}

//! 导出功能：把形状里的平面填充面写回 SOLID / 3DFACE。
//! 导入侧 SOLID/FACE3D 保留成面（见 DWGShape.cpp），只走边的话表格单元格、
//! 实心箭头这些"填充"会整块丢失（看图时就是一片空白）。
void appendFaces(const TopoDS_Shape& shape, const Entity& proto,
                 std::vector<Entity>& out)
{
    for (TopExp_Explorer explorer(shape, TopAbs_FACE); explorer.More();
         explorer.Next()) {
        const TopoDS_Face face = TopoDS::Face(explorer.Current());
        if (face.IsNull()) {
            continue;
        }
        BRepAdaptor_Surface surface(face);
        // 导出功能：面片不再"不是平面就丢"。导入时 SOLID/3DFACE/HATCH 的实心
        // 区域就是面，FreeCAD 里看得见（填充渲染）；一旦所在 compound 无效、
        // ShapeFix 修过之后，面的底层曲面会变成 BSpline，旧代码在这里直接
        // continue，导出就变成"实心变线框"。非平面面片改为三角化输出。
        const bool planarFace = (surface.GetType() == GeomAbs_Plane);
        std::vector<Vec3> corners;
        bool allStraight = true;
        for (TopExp_Explorer edges(face, TopAbs_EDGE); edges.More(); edges.Next()) {
            BRepAdaptor_Curve curve(TopoDS::Edge(edges.Current()));
            if (curve.GetType() != GeomAbs_Line) {
                allStraight = false;
                break;
            }
            const gp_Pnt start = curve.Value(curve.FirstParameter());
            corners.push_back(Vec3{start.X(), start.Y(), start.Z()});
        }
        // 导出功能：三角形用 3DFACE（3 点 SOLID 会退化成一条线，填充箭头
        // 就变成空心的）；四边形用 SOLID；更多顶点（多边形的实心填充，如
        // HATCH 的 SOLID 填充）三角化后逐片写 3DFACE，否则整块填充会丢。
        if (planarFace && allStraight && corners.size() == 3) {
            // 导出功能：实心填充必须用 SOLID（2D 填充实体）。3DFACE 是三维面，
            // 在二维线框视图里只画边不填充 —— 箭头/填充会变成空心的。
            Entity e = proto;
            e.type = EntType::SOLID;
            e.pts = corners;
            out.push_back(e);
            continue;
        }
        if (planarFace && allStraight && corners.size() == 4) {
            Entity e = proto;
            e.type = EntType::SOLID;  // 二维实心填充
            e.pts = corners;
            out.push_back(e);
            continue;
        }
        // 任意多边形面：用 OCC 三角化，再把相邻三角形两两拼成四边形（SOLID），
        // 剩下的写 3DFACE。这样每块填充的实体数大约减半，图纸不容易超过
        // "单个块"的实体上限（否则又会被按块拆分，失去逐线选中）。
        try {
            // 挠度按面的包围盒自适应：固定 0.5 mm 在 2e6 单位的大图纸上会把
            // 一块填充剖成上万个小三角形，实体数和文件体积都会爆掉。
            Bnd_Box faceBox;
            BRepBndLib::Add(face, faceBox);
            double diag = 0.0;
            if (!faceBox.IsVoid()) {
                const Bnd_Box& b = faceBox;
                const double dx = b.CornerMax().X() - b.CornerMin().X();
                const double dy = b.CornerMax().Y() - b.CornerMin().Y();
                const double dz = b.CornerMax().Z() - b.CornerMin().Z();
                diag = std::sqrt(dx * dx + dy * dy + dz * dz);
            }
            const double deflection = std::max(1e-4, diag * 2.0e-3);
            BRepMesh_IncrementalMesh mesher(face, deflection, Standard_False,
                                            deflection, Standard_True);
            TopLoc_Location loc;
            const Handle(Poly_Triangulation)& tri = BRep_Tool::Triangulation(face, loc);
            if (tri.IsNull()) {
                continue;
            }
            const gp_Trsf trsf = loc.Transformation();
            // 顶点去重：相邻三角形共用顶点编号才能判断"共边"
            std::map<std::tuple<long long, long long, long long>, int> index;
            std::vector<Vec3> points;
            auto pointIndex = [&](const gp_Pnt& p) {
                const long long key[3] = {(long long)std::llround(p.X() * 1000.0),
                                          (long long)std::llround(p.Y() * 1000.0),
                                          (long long)std::llround(p.Z() * 1000.0)};
                const auto k = std::make_tuple(key[0], key[1], key[2]);
                auto it = index.find(k);
                if (it != index.end()) {
                    return it->second;
                }
                const int id = (int)points.size();
                points.push_back(Vec3{p.X(), p.Y(), p.Z()});
                index[k] = id;
                return id;
            };

            struct Tri
            {
                int a, b, c;
                bool used = false;
            };
            std::vector<Tri> tris;
            for (Standard_Integer i = 1; i <= tri->NbTriangles(); ++i) {
                Standard_Integer n1 = 0, n2 = 0, n3 = 0;
                tri->Triangle(i).Get(n1, n2, n3);
                gp_Pnt p1 = tri->Node(n1);
                gp_Pnt p2 = tri->Node(n2);
                gp_Pnt p3 = tri->Node(n3);
                p1.Transform(trsf);
                p2.Transform(trsf);
                p3.Transform(trsf);
                Tri t;
                t.a = pointIndex(p1);
                t.b = pointIndex(p2);
                t.c = pointIndex(p3);
                if (face.Orientation() == TopAbs_REVERSED) {
                    std::swap(t.b, t.c);
                }
                tris.push_back(t);
            }

            // 共边合并：两个三角形拼成四边形（只合并凸的，避免填充溢出）
            auto cross = [](const Vec3& o, const Vec3& p, const Vec3& q) {
                return (p.x - o.x) * (q.y - o.y) - (p.y - o.y) * (q.x - o.x);
            };
            auto isConvexQuad = [&](int v0, int v1, int v2, int v3) {
                const double c1 = cross(points[v0], points[v1], points[v2]);
                const double c2 = cross(points[v1], points[v2], points[v3]);
                const double c3 = cross(points[v2], points[v3], points[v0]);
                const double c4 = cross(points[v3], points[v0], points[v1]);
                const bool anyPos = c1 > 0 || c2 > 0 || c3 > 0 || c4 > 0;
                const bool anyNeg = c1 < 0 || c2 < 0 || c3 < 0 || c4 < 0;
                return !(anyPos && anyNeg);
            };

            for (size_t i = 0; i < tris.size(); ++i) {
                if (tris[i].used) {
                    continue;
                }
                bool merged = false;
                for (size_t j = i + 1; j < tris.size() && !merged; ++j) {
                    if (tris[j].used) {
                        continue;
                    }
                    const int t1[3] = {tris[i].a, tris[i].b, tris[i].c};
                    const int t2[3] = {tris[j].a, tris[j].b, tris[j].c};
                    for (int e = 0; e < 3 && !merged; ++e) {
                        const int x = t1[(e + 1) % 3];
                        const int y = t1[(e + 2) % 3];
                        const int other1 = t1[e];
                        for (int k = 0; k < 3 && !merged; ++k) {
                            const int x2 = t2[k];
                            const int y2 = t2[(k + 1) % 3];
                            const int other2 = t2[(k + 2) % 3];
                            const bool sameEdge = (x == x2 && y == y2)
                                || (x == y2 && y == x2);
                            if (!sameEdge) {
                                continue;
                            }
                            // 四边形按顺序：other1 -> x -> other2 -> y
                            if (!isConvexQuad(other1, x, other2, y)) {
                                continue;
                            }
                            Entity quad = proto;
                            quad.type = EntType::SOLID;
                            quad.pts = {points[other1], points[x], points[other2],
                                        points[y]};
                            out.push_back(quad);
                            tris[i].used = true;
                            tris[j].used = true;
                            merged = true;
                        }
                    }
                }
                if (!merged) {
                    Entity tri1 = proto;
                    tri1.type = EntType::SOLID;
                    tri1.pts = {points[tris[i].a], points[tris[i].b],
                                points[tris[i].c], points[tris[i].c]};
                    out.push_back(tri1);
                    tris[i].used = true;
                }
            }
        }
        catch (const Standard_Failure&) {
            // 三角化失败：跳过这块填充，几何（边界线）仍然保留
        }
    }
}

// 导出功能：一个 shape 可能带着无效拓扑（导入侧个别多段线的 wire 不闭合/有
// 零长度边）。这种 shape 交给 OCC 遍历边会读到坏数据、把堆写坏，导出直接崩。
// 所以先整体检查：有效就整块导；无效就逐个子形状导，坏的先 ShapeFix，修不好
// 就只导出它的边（边本身通常是好的），保证"既不崩、也尽量不丢"。
void appendFaces(const TopoDS_Shape& shape, const Entity& proto,
                 std::vector<Entity>& out);
void appendEdges(const TopoDS_Shape& shape, const Entity& proto,
                 std::vector<Entity>& out);

static bool appendShapeRobust(const TopoDS_Shape& src, const Entity& proto,
                              std::vector<Entity>& out)
{
    if (src.IsNull()) {
        return false;
    }
    if (BRepCheck_Analyzer(src, Standard_False).IsValid()) {
        appendFaces(src, proto, out);
        appendEdges(src, proto, out);
        return true;
    }
    // 无效拓扑：逐个子形状导；坏的那块先 ShapeFix，修好再导，修不好就只导它的边。
    int bad = 0;
    for (TopoDS_Iterator it(src); it.More(); it.Next()) {
        const TopoDS_Shape& child = it.Value();
        std::vector<Entity> local;
        try {
            if (BRepCheck_Analyzer(child, Standard_False).IsValid()) {
                appendFaces(child, proto, local);
                appendEdges(child, proto, local);
            }
            else {
                ++bad;
                ShapeFix_Shape fixer(child);
                fixer.Perform();
                const TopoDS_Shape fixed = fixer.Shape();
                if (!fixed.IsNull()) {
                    appendFaces(fixed, proto, local);
                    appendEdges(fixed, proto, local);
                }
            }
        }
        catch (const Standard_Failure&) {
            // 这一块跳过，其余照常
        }
        out.insert(out.end(), local.begin(), local.end());
    }
    SWDWG_LOG("[SWDWG EXPORT] shape repaired piecewise (%d bad child(ren))\n", bad);
    return false;
}

// ============================ 导出功能 BEGIN ============================
//! 两点是否重合（合并线段用，1e-6 mm 容差）。
bool samePoint(const Vec3& a, const Vec3& b)
{
    return std::abs(a.x - b.x) <= 1e-6 && std::abs(a.y - b.y) <= 1e-6
        && std::abs(a.z - b.z) <= 1e-6;
}

//! 把首尾相接、同图层的连续 LINE 合并成一条 LWPOLYLINE。
//! 好处：实体数成倍下降（文件更小），并且不会再触发"单个块超过约 1.6 万
//! 实体就写坏"的按块拆分——不需要块，CAD 里就能逐条线悬停高亮/选中。
void mergeLineRuns(std::vector<Entity>& out, size_t from)
{
    if (out.size() < from + 2) {
        return;
    }
    std::vector<Entity> merged;
    merged.reserve(out.size());
    for (size_t i = 0; i < from; ++i) {
        merged.push_back(out[i]);
    }
    size_t i = from;
    while (i < out.size()) {
        if (out[i].type != EntType::LINE || out[i].pts.size() < 2) {
            merged.push_back(out[i]);
            ++i;
            continue;
        }
        size_t j = i;
        std::vector<Vec3> chain{out[i].pts[0], out[i].pts[1]};
        bool closesLoop = false;
        while (j + 1 < out.size() && out[j + 1].type == EntType::LINE
               && out[j + 1].pts.size() >= 2 && out[j + 1].layer == out[i].layer
               && samePoint(chain.back(), out[j + 1].pts[0])) {
            ++j;
            if (samePoint(chain.back(), out[j].pts[1])) {
                continue;  // 重复点，跳过
            }
            // 回到起点 = 闭合环。必须记下 closed，否则这一圈的最后一条边
            // （从末尾回到起点）会丢 —— 图框/表格边框这种矩形就会少一条边。
            if (chain.size() >= 3 && samePoint(chain.front(), out[j].pts[1])) {
                closesLoop = true;
                break;
            }
            chain.push_back(out[j].pts[1]);
        }
        if (chain.size() >= 3) {
            Entity pl = out[i];
            pl.type = EntType::LWPOLYLINE;
            pl.pts = chain;
            if (closesLoop) {
                pl.closed = true;
            }
            else if (chain.size() >= 4 && samePoint(chain.front(), chain.back())) {
                pl.closed = true;
                pl.pts.pop_back();  // 闭合多段线不重复首点
            }
            merged.push_back(pl);
        }
        else {
            for (size_t k = i; k <= j; ++k) {
                merged.push_back(out[k]);
            }
        }
        i = j + 1;
    }
    out.swap(merged);
}
// ============================ 导出功能 END ==============================

//! 导出功能：把一层里的所有边追加成 LINE / ARC / CIRCLE / ELLIPSE。
//! 非解析曲线（B 样条、退化圆锥）按折线采样，保证几何不丢。
void appendEdges(const TopoDS_Shape& shape, const Entity& proto,
                 std::vector<Entity>& out)
{
    for (TopExp_Explorer explorer(shape, TopAbs_EDGE); explorer.More();
         explorer.Next()) {
        const TopoDS_Edge edge = TopoDS::Edge(explorer.Current());
        if (edge.IsNull()) {
            continue;
        }
        BRepAdaptor_Curve curve(edge);
        const double first = curve.FirstParameter();
        const double last = curve.LastParameter();
        if (!(last > first)) {
            continue;
        }

        switch (curve.GetType()) {
            case GeomAbs_Line: {
                const gp_Pnt a = curve.Value(first);
                const gp_Pnt b = curve.Value(last);
                Entity e = proto;
                e.type = EntType::LINE;
                e.pts = {Vec3{a.X(), a.Y(), a.Z()}, Vec3{b.X(), b.Y(), b.Z()}};
                out.push_back(e);
                break;
            }
            case GeomAbs_Circle: {
                const gp_Circ circle = curve.Circle();
                const gp_Pnt center = circle.Location();
                const gp_Dir axis = circle.Axis().Direction();
                // DWG ARC/CIRCLE live in the XY plane (OCS +Z); a tilted circle
                // is sampled instead of written wrong.
                const bool planar = std::fabs(axis.Z()) > 0.999;
                // 导出功能：判断"整圆"必须看这条**边**的参数跨度，不能用
                // curve.IsClosed() —— BRepAdaptor_Curve::IsClosed() 反映的是
                // 底层曲线（圆本来就闭合），于是每一段圆弧边都被写成整圆，
                // 导出图里就会凭空多出一堆大圆圈（用户报的现象）。
                const bool fullCircle = (last - first) >= 2.0 * kPi - 1e-6;
                if (planar && fullCircle) {
                    Entity e = proto;
                    e.type = EntType::CIRCLE;
                    e.center = Vec3{center.X(), center.Y(), center.Z()};
                    e.radius = circle.Radius();
                    out.push_back(e);
                }
                else if (planar) {
                    // 导出功能：圆心角一律用**边的参数跨度**（圆的参数就是弧度），
                    // 不能由两个端点的 atan2 相减去凑。源图里存在"起点终点重合、
                    // 但跨度是 0"的退化圆弧，用端点算会把 sweep 补成 2π，写成
                    // start==end 的 ARC —— CAD 会把它画成一整个大圆，导出图里就
                    // 凭空多出大圆圈（用户报的现象）。
                    const double span = last - first;
                    if (span < 1e-3) {
                        // 退化到几乎为零的圆弧（源图里的残留数据）。CAD 把
                        // start==end 的 ARC 画成**整圆**，这里改写成一条极短
                        // 的直线，既不留大圆圈，也不丢几何。
                        const gp_Pnt a0 = curve.Value(first);
                        const gp_Pnt b0 = curve.Value(last);
                        Entity e = proto;
                        e.type = EntType::LINE;
                        e.pts = {Vec3{a0.X(), a0.Y(), a0.Z()},
                                 Vec3{b0.X(), b0.Y(), b0.Z()}};
                        out.push_back(e);
                        break;
                    }
                    const gp_Pnt a = curve.Value(first);
                    const gp_Pnt b = curve.Value(last);
                    const double a1 =
                        std::atan2(a.Y() - center.Y(), a.X() - center.X());
                    const double a2 =
                        std::atan2(b.Y() - center.Y(), b.X() - center.X());
                    double delta = a2 - a1;
                    while (delta < 0.0) {
                        delta += 2.0 * kPi;
                    }
                    while (delta >= 2.0 * kPi) {
                        delta -= 2.0 * kPi;
                    }
                    // 正向（a1 -> a1+span）是否落在 b 上；不是就从 b 起算
                    const bool forward = std::fabs(delta - span) <= std::fabs(2.0 * kPi - delta - span);
                    const double start = forward ? a1 : a2;
                    Entity e = proto;
                    e.type = EntType::ARC;
                    e.center = Vec3{center.X(), center.Y(), center.Z()};
                    e.radius = circle.Radius();
                    e.startAngle = start;
                    e.endAngle = start + span;
                    out.push_back(e);
                }
                else {
                    // sampled fallback
                    const int steps = 32;
                    const gp_Pnt previous = curve.Value(first);
                    Entity e = proto;
                    e.type = EntType::LINE;
                    for (int i = 1; i <= steps; ++i) {
                        const gp_Pnt p =
                            curve.Value(first + (last - first) * i / steps);
                        e.pts = {Vec3{previous.X(), previous.Y(), previous.Z()},
                                 Vec3{p.X(), p.Y(), p.Z()}};
                        out.push_back(e);
                        e.pts[0] = e.pts[1];
                    }
                }
                break;
            }
            case GeomAbs_Ellipse: {
                const gp_Elips ellipse = curve.Ellipse();
                const gp_Pnt center = ellipse.Location();
                const gp_Dir axis = ellipse.Axis().Direction();
                const bool fullEllipse = (last - first) >= 2.0 * kPi - 1e-6;
                if (std::fabs(axis.Z()) > 0.999 && fullEllipse) {
                    const gp_Dir major = ellipse.XAxis().Direction();
                    Entity e = proto;
                    e.type = EntType::ELLIPSE;
                    e.center = Vec3{center.X(), center.Y(), center.Z()};
                    e.radius = ellipse.MajorRadius();
                    e.axisRatio = ellipse.MajorRadius() > 1e-12
                                      ? ellipse.MinorRadius() / ellipse.MajorRadius()
                                      : 1.0;
                    e.smAxis = Vec3{major.X() * ellipse.MajorRadius(),
                                    major.Y() * ellipse.MajorRadius(),
                                    major.Z() * ellipse.MajorRadius()};
                    out.push_back(e);
                }
                else {
                    const int steps = 48;
                    Entity e = proto;
                    e.type = EntType::LINE;
                    gp_Pnt previous = curve.Value(first);
                    for (int i = 1; i <= steps; ++i) {
                        const gp_Pnt p =
                            curve.Value(first + (last - first) * i / steps);
                        e.pts = {Vec3{previous.X(), previous.Y(), previous.Z()},
                                 Vec3{p.X(), p.Y(), p.Z()}};
                        out.push_back(e);
                        previous = p;
                    }
                }
                break;
            }
            default: {
                // B-splines and everything else: polyline sampling.
                int steps = 32;
                const double span = last - first;
                steps = std::max(8, std::min(128, (int)std::ceil(span / 2.0)));
                Entity e = proto;
                e.type = EntType::LINE;
                gp_Pnt previous = curve.Value(first);
                for (int i = 1; i <= steps; ++i) {
                    const gp_Pnt p = curve.Value(first + span * i / steps);
                    e.pts = {Vec3{previous.X(), previous.Y(), previous.Z()},
                             Vec3{p.X(), p.Y(), p.Z()}};
                    out.push_back(e);
                    previous = p;
                }
                break;
            }
        }
    }
}

//! 导出功能：一个 DWG 图层（同名图层只建一次）。
DWGOutLayer& layerEntry(std::map<std::string, DWGOutLayer>& layers,
                        std::vector<DWGOutLayer>& order, const std::string& name,
                        int aci, int lineWeight, const std::string& ltype)
{
    auto it = layers.find(name);
    if (it != layers.end()) {
        return it->second;
    }
    DWGOutLayer layer;
    layer.name = name.empty() ? "0" : name;
    layer.colorIndex = aci > 0 ? aci : 7;
    layer.lineWeight = lineWeight;
    layer.ltype = ltype.empty() ? "CONTINUOUS" : ltype;
    order.push_back(layer);
    auto inserted = layers.emplace(layer.name, layer);
    return inserted.first->second;
}

//! 导出功能：把对象的世界变换补到形状上。
//!
//! 本分支的 Shape 往往已经把 Placement 烘进形状的 Location（大坐标相关改动），
//! 这时再乘一次会让坐标翻倍（例如 y=-12000 变成 -24000）。所以只有当形状本身
//! 没有 Location 时才补全局 Placement。
void applyGlobalPlacement(Part::TopoShape& shape, App::DocumentObject* obj)
{
    const TopoDS_Shape raw = shape.getShape();
    if (!raw.IsNull() && !raw.Location().IsIdentity()) {
        return;  // 已经带位置，不要再乘一次
    }
    const Base::Placement global = App::GeoFeature::getGlobalPlacement(obj);
    if (global.isIdentity()) {
        return;
    }
    shape.transformShape(global.toMatrix(), true);
}

//! 导出功能：Annotation 的文字行合并成一个字符串（MTEXT 用 \P 换行）。
std::string annotationText(const App::Annotation* ann, bool mtext)
{
    const std::vector<std::string> lines = ann->LabelText.getValues();
    std::string text;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i) {
            text += mtext ? "\\P" : "\n";
        }
        text += lines[i];
    }
    return text;
}

// ============================ 导出功能 BEGIN ============================
//! 解析 DWGLineType.Pattern（"12.7,-6.35" 这种，正=划线、负=空白、0=点），
//! 只保留简单的数值段；带 s/t 的复合元素（形/文字）跳过。
bool parseDashPattern(const std::string& text, std::vector<double>& out)
{
    out.clear();
    const char* p = text.c_str();
    while (p && *p) {
        while (*p == ',' || *p == ' ' || *p == '\t' || *p == ';') {
            ++p;
        }
        if (!*p) {
            break;
        }
        if (*p == 's' || *p == 'S' || *p == 't' || *p == 'T' || *p == 'x'
            || *p == 'X') {
            return false;  // 复合线型（形/文字），导出暂不支持
        }
        char* end = nullptr;
        const double value = std::strtod(p, &end);
        if (end == p) {
            return false;
        }
        out.push_back(value);
        p = end;
    }
    return !out.empty();
}

//! 导出功能：收集文档里的线型定义（SWDWG::DWGLineType -> LTYPE 表记录）。
void collectLinetypes(App::Document* doc,
                      std::vector<DWGOutLinetype>& linetypes)
{
    std::set<std::string> seen;
    for (const auto& lt : linetypes) {
        seen.insert(lt.name);
    }
    for (App::DocumentObject* obj : doc->getObjects()) {
        auto* lt = dynamic_cast<SWDWG::DWGLineType*>(obj);
        if (!lt) {
            continue;
        }
        const std::string name = lt->Name.getValue();
        if (name.empty() || name == "CONTINUOUS" || name == "ByLayer"
            || name == "BYLAYER" || seen.count(name)) {
            continue;
        }
        DWGOutLinetype out;
        out.name = name;
        if (!parseDashPattern(lt->Pattern.getValue(), out.dashes)) {
            continue;  // 没有可用图案（或复合线型）→ 实体会落到 BYLAYER
        }
        out.description = name + " (SWDWG export)";
        for (double d : out.dashes) {
            out.patternLength += std::abs(d);
        }
        linetypes.push_back(out);
        seen.insert(name);
    }
}
// ============================ 导出功能 END ==============================

}  // namespace

int DWGCompose::compose(App::Document* doc,
                        const std::vector<App::DocumentObject*>& objects,
                        std::vector<Entity>& entities,
                        std::vector<DWGOutLayer>& layers,
                        std::vector<DWGOutLinetype>& linetypes,
                        std::string& error)
{
    error.clear();
    // 导出功能：先把线型表收集好（虚线/点划线），后面按实体挂上去
    collectLinetypes(doc, linetypes);
    if (!doc) {
        error = "导出失败：没有文档";
        return 0;
    }

    const std::vector<App::DocumentObject*> list =
        objects.empty() ? doc->getObjects() : objects;

    std::map<std::string, DWGOutLayer> layerMap;
    int skipped = 0;

    for (App::DocumentObject* obj : list) {
        if (!obj) {
            ++skipped;
            continue;
        }

        // --- 线组（导入器建的图层聚合对象）：图层/颜色/线宽/线型 + 原始几何
        if (auto* group = dynamic_cast<SWDWG::DWGLineGroup*>(obj)) {
            const std::string layerName = group->Layer.getValue();
            const Base::Color color = group->DWGColor.getValue();
            const int aci = DWGWriter::rgbToAci(packedRgb(color));
            const int weight =
                (int)std::lround(group->DWGLineWidth.getValue() * 100.0);
            DWGOutLayer& layer = layerEntry(layerMap, layers, layerName, aci,
                                            weight, group->DWGLineType.getValue());

            Entity proto;
            proto.layer = layer.name;
            // 导出功能：实体自带颜色 —— 同一图层上可能有多组不同颜色，
            // 只靠图层颜色会把整张图刷成一种颜色。这里写实体的真彩色/ACI。
            proto.rgb = packedRgb(color);
            proto.colorIndex = aci;
            proto.lineWeight = weight;
            // 导出功能：线型按**实体**写（同一图层上会有不同线型的组），
            // 只写图层线型的话，层 0 上的虚线圆会退化成实线。
            proto.ltype = group->DWGLineType.getValue();
            if (proto.ltype.empty()) {
                proto.ltype = layer.ltype;
            }
            proto.ltypeScale = group->DWGLineScale.getValue();

            Part::TopoShape shape = group->DWGSourceShape.getShape();
            applyGlobalPlacement(shape, obj);
            const size_t before = entities.size();
            // 导出功能：导入器的个别多段线可能留下无效拓扑（重合点造成的零长度
            // 边）。直接遍历这种 shape 的边会让 OCC 越界读取、把堆写坏，最终
            // 表现为"导出报错 + FreeCAD 退出"。这里先检查，无效就用 ShapeFix
            // 修一遍，修不好就跳过（宁可少这一块，也不能崩）。
            TopoDS_Shape src = shape.getShape();
            if (appendShapeRobust(src, proto, entities)) {
                // 整个 shape 有效，正常导出（绝大多数情况）
            }
            // 导出功能：相邻线段合并成多段线，减少实体数（避免按块拆分）
            mergeLineRuns(entities, before);
            if (entities.size() == before) {
                ++skipped;
            }
            continue;
        }

        // --- 文字
        if (auto* text = dynamic_cast<SWDWG::DWGText*>(obj)) {
            const bool isMText = text->IsMText.getValue();
            Entity e;
            e.type = isMText ? EntType::MTEXT : EntType::TEXT;
            e.layer = text->Layer.getValue();
            const int aci = DWGWriter::rgbToAci(packedRgb(text->TextColor.getValue()));
            // 导出功能：文字颜色按实体写（真彩色），不要被图层颜色覆盖
            e.rgb = packedRgb(text->TextColor.getValue());
            e.colorIndex = aci;
            layerEntry(layerMap, layers, e.layer, aci, -1, "CONTINUOUS");
            e.text = annotationText(text, isMText);
            e.textHeight = text->FontHeight.getValue();
            e.insPt = Vec3{text->Position.getValue().x,
                           text->Position.getValue().y,
                           text->Position.getValue().z};
            // Rotation：Rotation 属性是权威值（Position 里通常不带旋转）
            e.rotation = text->Rotation.getValue();
            e.textHAlign = text->HorAlign.getValue();
            e.textVAlign = text->VertAlign.getValue();
            // ===================== 导出功能 BEGIN =====================
            // TEXT 有原始插入点（DXF 10）时按左/基线原样写回 —— 这个点就是文字框
            // 左下角，位置与源图完全一致，不用再按字宽估算对齐偏移。
            if (!isMText) {
                const Base::Vector3d rawIns = text->InsPt.getValue();
                if (rawIns.Length() > 1e-9) {
                    e.insPt = Vec3{rawIns.x, rawIns.y, rawIns.z};
                    e.textHAlign = 0;
                    e.textVAlign = 0;
                }
            }
            // ===================== 导出功能 END =======================
            e.mtextAttachment = text->Attachment.getValue();
            e.textAttrs.widthFactor = text->WidthFactor.getValue();
            e.textAttrs.oblique = text->ObliqueAngle.getValue() * 180.0 / kPi;
            e.textAttrs.styleName = text->StyleName.getValue();
            e.textAttrs.lsFactor = text->LineSpacing.getValue();
            // 导出功能：带上样式字体（含 CJK 大字体），否则中文会变成 "?"
            e.textAttrs.fontFile = text->FontFile.getValue();
            e.textAttrs.bigFontFile = text->BigFontFile.getValue();
            entities.push_back(e);
            continue;
        }

        // --- 尺寸标注：导入时已展开成显示几何，这里按几何 + 文字回写
        if (auto* dim = dynamic_cast<SWDWG::DWGDimension*>(obj)) {
            const std::string layerName = dim->Layer.getValue();
            const int aci = DWGWriter::rgbToAci(packedRgb(dim->DimColor.getValue()));
            DWGOutLayer& layer =
                layerEntry(layerMap, layers, layerName, aci, -1, "CONTINUOUS");

            Entity proto;
            proto.layer = layer.name;
            // 导出功能：尺寸线/箭头用尺寸颜色（真彩色），文字用文字颜色
            proto.rgb = packedRgb(dim->DimColor.getValue());
            proto.colorIndex = aci;

            const std::vector<Base::Vector3d> dimLines = dim->DimLines.getValues();
            const std::vector<Base::Vector3d> extLines = dim->ExtLines.getValues();
            auto addSegments = [&entities, &proto](const std::vector<Base::Vector3d>& pts) {
                for (size_t i = 0; i + 1 < pts.size(); i += 2) {
                    Entity e = proto;
                    e.type = EntType::LINE;
                    e.pts = {Vec3{pts[i].x, pts[i].y, pts[i].z},
                             Vec3{pts[i + 1].x, pts[i + 1].y, pts[i + 1].z}};
                    entities.push_back(e);
                }
            };
            addSegments(dimLines);
            addSegments(extLines);

            const std::vector<Base::Vector3d> arrows = dim->ArrowTriangles.getValues();
            for (size_t i = 0; i + 2 < arrows.size(); i += 3) {
                Entity e = proto;
                // 导出功能：箭头是实心三角，必须写 SOLID（二维填充实体）；
                // 3DFACE 在二维线框视图里只画边，箭头会变空心。
                e.type = EntType::SOLID;
                e.pts = {Vec3{arrows[i].x, arrows[i].y, arrows[i].z},
                         Vec3{arrows[i + 1].x, arrows[i + 1].y, arrows[i + 1].z},
                         Vec3{arrows[i + 2].x, arrows[i + 2].y, arrows[i + 2].z}};
                entities.push_back(e);
            }

            const std::string text = dim->DimText.getValue();
            if (!text.empty()) {
                Entity e = proto;
                // ===================== 导出功能 BEGIN =====================
                // 尺寸文字用 MTEXT 写：MTEXT 的 attachment（5 = 正中）由 CAD
                // 自己按文字框居中，不依赖 alignment_pt，也不用估字宽。TEXT
                // 的居中只能靠估宽补偿，实测会偏半个字（标注看位置不准）。
                e.type = EntType::MTEXT;
                e.mtextAttachment = 5;  // middle center
                e.textAttrs.mtextRectW = 0.0;  // 不自动换行
                // ===================== 导出功能 END =======================
                e.text = text;
                e.textHeight = dim->TextHeight.getValue();
                e.mtextHeight = dim->TextHeight.getValue();
                const Base::Vector3d pos = dim->TextPosition.getValue();
                e.insPt = Vec3{pos.x, pos.y, pos.z};
                e.rotation = dim->TextRotation.getValue();
                e.textHAlign = 1;  // center
                e.textVAlign = 2;  // middle
                // 导出功能：尺寸文字的颜色/字体/大字体，缺了就变问号
                e.rgb = packedRgb(dim->TextColor.getValue());
                e.colorIndex = DWGWriter::rgbToAci(e.rgb);
                e.textAttrs.fontFile = dim->FontFile.getValue();
                e.textAttrs.bigFontFile = dim->BigFontFile.getValue();
                e.textAttrs.styleName = dim->TextStyleName.getValue();
                e.textAttrs.widthFactor =
                    dim->TextWidthFactor.getValue() > 0.0
                        ? dim->TextWidthFactor.getValue()
                        : 1.0;
                entities.push_back(e);
            }
            continue;
        }

        // --- 曲面网格：按面片写 3DFACE
        if (auto* mesh = dynamic_cast<SWDWG::DWGSurfaceMesh*>(obj)) {
            const std::string layerName = mesh->Layer.getValue();
            const int aci = DWGWriter::rgbToAci(packedRgb(mesh->MeshColor.getValue()));
            DWGOutLayer& layer =
                layerEntry(layerMap, layers, layerName, aci, -1, "CONTINUOUS");

            const std::vector<Base::Vector3d> pts = mesh->Points.getValues();
            const std::vector<long> faces = mesh->FaceIndex.getValues();
            for (size_t i = 0; i + 3 < faces.size(); i += 4) {
                Entity e;
                e.type = EntType::FACE3D;
                e.layer = layer.name;
                e.rgb = packedRgb(mesh->MeshColor.getValue());
                e.colorIndex = aci;
                for (int k = 0; k < 4; ++k) {
                    const long index = faces[i + k];
                    if (index < 0 || index >= (long)pts.size()) {
                        continue;
                    }
                    e.pts.push_back(Vec3{pts[index].x, pts[index].y, pts[index].z});
                }
                if (e.pts.size() >= 3) {
                    entities.push_back(e);
                }
            }
            continue;
        }

        // --- 普通对象：任何带 Shape 的对象（Part::Feature、Draft 线/圆/多边形…）
        auto* shapeProp = dynamic_cast<Part::PropertyPartShape*>(
            obj->getPropertyByName("Shape"));
        if (shapeProp) {
            Part::TopoShape shape = shapeProp->getShape();
            applyGlobalPlacement(shape, obj);
            const size_t before = entities.size();

            const std::string layerName = obj->getNameInDocument();
            DWGOutLayer& layer =
                layerEntry(layerMap, layers, layerName, 7, -1, "CONTINUOUS");

            Entity proto;
            proto.layer = layer.name;
            proto.colorIndex = 256;
            // 普通 Part 对象只写边线：写面会变成实心填充，把 2D 图盖住。
            appendEdges(shape.getShape(), proto, entities);
            mergeLineRuns(entities, before);
            if (entities.size() == before) {
                ++skipped;
            }
            continue;
        }

        // --- 兜底：App::Annotation（Draft 文字等）按 TEXT 输出
        if (auto* ann = dynamic_cast<App::Annotation*>(obj)) {
            const std::string text = annotationText(ann, false);
            if (text.empty()) {
                ++skipped;
                continue;
            }
            Entity e;
            e.type = EntType::TEXT;
            e.layer = "0";
            e.colorIndex = 256;
            layerEntry(layerMap, layers, e.layer, 7, -1, "CONTINUOUS");
            e.text = text;
            // Annotation 没有字高属性（字号在视图层），按 2.5 mm 兜底并记录
            e.textHeight = 2.5;
            if (auto* heightProp =
                    dynamic_cast<App::PropertyFloat*>(obj->getPropertyByName("FontHeight"))) {
                if (heightProp->getValue() > 0.0) {
                    e.textHeight = heightProp->getValue();
                }
            }
            const Base::Vector3d pos = ann->Position.getValue();
            e.insPt = Vec3{pos.x, pos.y, pos.z};
            entities.push_back(e);
            continue;
        }

        ++skipped;
    }

    SWDWG_LOG("[SWDWG EXPORT] composed %d entities, %d layers, %d objects skipped\n",
              (int)entities.size(), (int)layers.size(), skipped);
    return skipped;
}

}  // namespace SWDWG
