#include "PreCompiled.h"

#include <chrono>
#include <cstring>
#include <sstream>

#include <Inventor/nodes/SoAsciiText.h>
#include <Inventor/nodes/SoBaseColor.h>
#include <Inventor/nodes/SoCoordinate3.h>
#include <Inventor/nodes/SoFont.h>
#include <Inventor/nodes/SoGroup.h>
#include <Inventor/nodes/SoIndexedLineSet.h>
#include <Inventor/nodes/SoLightModel.h>
#include <Inventor/nodes/SoMatrixTransform.h>
#include <Inventor/nodes/SoSeparator.h>
#include <Inventor/nodes/SoTransform.h>

#include <App/Application.h>
#include <App/Document.h>
#include <App/PropertyStandard.h>
#include <App/PropertyUnits.h>
#include <Base/Console.h>
#include <Base/Parameter.h>

#include <Gui/Selection/SoFCSelection.h>

#include "ViewProviderDWGText.h"
#include "../App/DWGText.h"
#include "../App/TextShxRenderer.h"

using namespace SWDWG;

namespace
{

//! True when a font / big-font file name is a classic AutoCAD .shx.
bool isShxFontName(const std::string& s)
{
    const std::string::size_type dot = s.find_last_of('.');
    if (dot == std::string::npos) {
        return false;
    }
    std::string ext = s.substr(dot + 1);
    for (char& c : ext) {
        c = static_cast<char>(std::tolower((unsigned char)c));
    }
    return ext == "shx";
}

//! Diagnostics probe (default off): when set through the SWDWG preference
//! group, TrueType texts do NOT get an SoAsciiText node.  Used to measure how
//! much of the .fcstd open time is Coin's per-glyph font rasterisation of
//! SoAsciiText vs. our own (near-free) polyline scene building.
bool probeSkipTtfNodes()
{
    static const bool skip = []() {
        const Base::Reference<ParameterGrp> hGrp =
            App::GetApplication().GetParameterGroupByPath(
                "User parameter:BaseApp/Preferences/Mod/SWDWG");
        return hGrp ? hGrp->GetBool("ProbeSkipTTF", false) : false;
    }();
    return skip;
}

//! True when the text is actually stroked by the .shx engine (some .shx font
//! file was located), as opposed to the TrueType / SoAsciiText fallback.
//! The .shx engine bakes WidthFactor into the stroke coordinates it emits
//! (see addShxLine), so the per-line scale node must NOT apply wf again;
//! the SoAsciiText path has no internal wf and relies on that scale node.
bool engineIsShx(const DWGText* anno)
{
    if (!anno) {
        return false;
    }
    return isShxFont(anno->FontFile.getValue(),
                     anno->BigFontFile.getValue());
}

//! Append one glyph's stroke segments to a Coin polyline buffer.
//! Segments are shifted by (ox, oy) before being added.  Zero-length
//! segments (Coin cannot draw them) are skipped.
void appendGlyphSegs(const std::vector<ShapeSeg>& segs, double ox, double oy,
                     std::vector<SbVec3f>& pts, std::vector<int32_t>& idx)
{
    for (const ShapeSeg& s : segs) {
        if ((s.x1 - s.x0) * (s.x1 - s.x0)
                + (s.y1 - s.y0) * (s.y1 - s.y0)
            < 1e-12) {
            continue;
        }
        const int32_t base = static_cast<int32_t>(pts.size());
        pts.emplace_back(static_cast<float>(s.x0 + ox),
                         static_cast<float>(s.y0 + oy), 0.0f);
        pts.emplace_back(static_cast<float>(s.x1 + ox),
                         static_cast<float>(s.y1 + oy), 0.0f);
        idx.push_back(base);
        idx.push_back(base + 1);
        idx.push_back(-1);
    }
}

//! Build a fresh BASE_COLOR line node set from a point/index buffer and add
//! it to \a group.  Coin3D detaches a node from its previous parent when it
//! is added to another group, so every call must use a new node set.
void addLineNodes(SoGroup* group, std::vector<SbVec3f>& pts,
                  std::vector<int32_t>& idx)
{
    auto* light = new SoLightModel();
    light->model = SoLightModel::BASE_COLOR;
    auto* coords = new SoCoordinate3();
    auto* lines = new SoIndexedLineSet();
    coords->point.setNum(static_cast<int>(pts.size()));
    coords->point.setValues(0, static_cast<int>(pts.size()), pts.data());
    lines->coordIndex.setNum(static_cast<int>(idx.size()));
    lines->coordIndex.setValues(0, static_cast<int>(idx.size()), idx.data());
    group->addChild(light);
    group->addChild(coords);
    group->addChild(lines);
}

}  // namespace

// ---------------------------------------------------------------------------
// Restore-time diagnostics: how much of the per-text open cost is the SHX
// glyph stroking (renderShxLine) vs. everything else (scene node building +
// App property restore).  Reset per document restore, printed by AppSWDWGGui.
// ---------------------------------------------------------------------------
namespace
{

double g_vpRebuildSec = 0.0;  // total time inside rebuildLines()
double g_vpShxSec = 0.0;      // of which spent inside renderShxLine()
int g_vpRebuilds = 0;         // number of rebuildLines() calls
double g_vpAttachSec = 0.0;   // total time inside attach() (incl. base class)
int g_vpAttaches = 0;         // number of attach() calls

class VpRebuildWatch
{
public:
    VpRebuildWatch()
        : start_(std::chrono::steady_clock::now())
    {
    }
    ~VpRebuildWatch()
    {
        g_vpRebuildSec += std::chrono::duration<double>(
                              std::chrono::steady_clock::now() - start_)
                              .count();
        ++g_vpRebuilds;
    }

private:
    std::chrono::steady_clock::time_point start_;
};

}  // namespace

void swdwg_resetOpenTimers()
{
    g_vpRebuildSec = 0.0;
    g_vpShxSec = 0.0;
    g_vpRebuilds = 0;
    g_vpAttachSec = 0.0;
    g_vpAttaches = 0;
}
double swdwg_vpRebuildSec()
{
    return g_vpRebuildSec;
}
double swdwg_vpShxSec()
{
    return g_vpShxSec;
}
int swdwg_vpRebuilds()
{
    return g_vpRebuilds;
}
double swdwg_vpAttachSec()
{
    return g_vpAttachSec;
}
int swdwg_vpAttaches()
{
    return g_vpAttaches;
}

PROPERTY_SOURCE(SWDWG::ViewProviderDWGText, Gui::ViewProviderDocumentObject)

ViewProviderDWGText::ViewProviderDWGText()
{
    pTransform = new SoTransform();
    pTransform->ref();
    pColor = new SoBaseColor();
    pColor->ref();
    pFont = new SoFont();
    pFont->ref();
    pLineGroup = new SoSeparator();
    pLineGroup->ref();

    sPixmap = "Tree_Annotation";
}

ViewProviderDWGText::~ViewProviderDWGText()
{
    pTransform->unref();
    pColor->unref();
    pFont->unref();
    pLineGroup->unref();
}

void ViewProviderDWGText::attach(App::DocumentObject* f)
{
    const auto attachT0 = std::chrono::steady_clock::now();
    ViewProviderDocumentObject::attach(f);

    auto* textsep = new Gui::SoFCSelection();
    textsep->objectName = pcObject->getNameInDocument();
    textsep->documentName = pcObject->getDocument()->getName();
    textsep->subElementName = "Main";

    // Order matters: transform (anchor + rotation) -> color -> font -> lines.
    textsep->addChild(pTransform);
    textsep->addChild(pColor);
    textsep->addChild(pFont);
    textsep->addChild(pLineGroup);

    // ★ 强制开启渲染缓存（与 Infra3D_Roadway ViewProviderHorizontalAlignment 的
    //    SoAnnotation 标注层同一策略）：
    //    SoAsciiText 每次渲染都要用 FreeType 生成字形，大量文字时若缓存
    //    关闭，任何视图操作（旋转/缩放/选择高亮）都会触发全量字形重建 → 卡顿。
    //    renderCaching=ON 把文字渲染冻结为 display list，相机变化时直接重放，
    //    选择高亮导致父级 pcRoot 重建时此级缓存仍有效。
    pRoot = new SoSeparator();
    pRoot->addChild(textsep);
    pRoot->renderCaching = SoSeparator::ON;
    pRoot->boundingBoxCaching = SoSeparator::ON;
    addDisplayMaskMode(pRoot, "3D");

    // Push the current values of the App properties into the scene graph.
    auto* anno = dynamic_cast<DWGText*>(f);
    if (anno) {
        const Base::Color& c = anno->TextColor.getValue();
        pColor->rgb.setValue(c.r, c.g, c.b);
        pFont->size = anno->FontHeight.getValue();
        pFont->name = anno->FontName.getValue();
        Base::Vector3d v = anno->Position.getValue();
        pTransform->translation.setValue((float)v.x, (float)v.y, (float)v.z);
        double angle = anno->Rotation.getValue();
        pTransform->rotation.setValue(SbVec3f(0.0f, 0.0f, 1.0f),
                                      (float)angle);
        updateScale();
        updateShear();
        // During a bulk import the importer's window is already open while
        // addObject() attaches this object; the properties are not filled yet
        // so building glyphs now would be wasted work (and would run 10+ more
        // times as FreeCAD touches each property).  The importer's closing
        // recomputeLayoutOnce() triggers the single rebuild that matters.
        // The same deferral applies while a .fcstd document is being restored:
        // FreeCAD writes the properties back one by one and the glyphs must
        // only be rebuilt once, in finishRestoring(), when all of them are in.
        if (!deferHeavyRebuild()) {
            rebuildLines();
        }
    }
    g_vpAttachSec += std::chrono::duration<double>(
                         std::chrono::steady_clock::now() - attachT0)
                         .count();
    ++g_vpAttaches;
}

bool ViewProviderDWGText::deferHeavyRebuild()
{
    if (DWGText::isBulkCreating()) {
        return true;  // importer window: filled after leaveBulkCreate()
    }
    if (isRestoring()) {  // Gui::isRestoring status bit
        return true;
    }
    const App::DocumentObject* o = pcObject;
    return o && (o->isRestoring() || App::Document::isAnyRestoring());
}

void ViewProviderDWGText::finishRestoring()
{
    // All App + view properties are restored now; build the glyph nodes a
    // single time instead of once per property during updateData.
    auto* anno = dynamic_cast<DWGText*>(pcObject);
    if (anno) {
        if (!anno->LabelText.getValues().empty()
            && anno->LabelOffsets.getValues().empty()) {
            // Legacy / hand-written file without a persisted layout: compute
            // the offsets first.  The updateData that fires from the property
            // write may still be deferred while the surrounding document is
            // restoring, so the unconditional rebuild below is what renders.
            anno->recomputeLayoutOnce();
        }
    }
    rebuildLines();
    ViewProviderDocumentObject::finishRestoring();
}

void ViewProviderDWGText::rebuildLines()
{
    VpRebuildWatch _watch;
    auto* anno = dynamic_cast<DWGText*>(pcObject);
    if (!anno) {
        return;
    }
    // Drop the previous line sub-trees.
    while (pLineGroup->getNumChildren() > 0) {
        pLineGroup->removeChild(pLineGroup->getChild(0));
    }
    m_lineScales.clear();
    m_lineShears.clear();

    const std::vector<std::string>& rawLines = anno->LabelText.getValues();
    const std::vector<Base::Vector3d>& offs = anno->LabelOffsets.getValues();
    const bool vertical = anno->IsVertical.getValue();
    const float negRot = vertical
        ? static_cast<float>(-anno->Rotation.getValue())
        : 0.0f;

    // For vertical text the LabelOffsets were computed per character; split
    // the display text to match without changing the stored LabelText.
    std::vector<std::string> lines = rawLines;
    if (vertical && !rawLines.empty()) {
        std::string all;
        all.reserve(rawLines.size() * 8);
        for (const std::string& ln : rawLines) {
            all += ln;
        }
        lines = splitUtf8Chars(all);
    }
    if (vertical) {
        std::ostringstream oss;
        oss << "[SWDWG VRENDER] vertical=1 rawLines=" << rawLines.size()
            << " chars=" << lines.size() << " offs=" << offs.size()
            << " rot=" << anno->Rotation.getValue()
            << " (rendered as one multi-line SoAsciiText)";
        for (const std::string& ln : lines) {
            oss << " '" << ln << "'";
        }
        SWDWG_LOG("%s\n", oss.str().c_str());
    }

    // Vertical text is rendered as one multi-line SoAsciiText node, with one
    // line per Unicode character.  This avoids the per-character metrics
    // inconsistencies that occur when each glyph is a separate SoAsciiText
    // node (visible as an increasing gap between Chinese characters).  The
    // column is counter-rotated as a whole so the glyphs stay upright.
    if (vertical && !lines.empty()) {
        // The column gets its own separator.  In Coin a transform is state that
        // keeps applying to the FOLLOWING siblings of its parent, so the offset
        // / counter-rotation / scale / shear must be scoped to the column and
        // not leak into whatever is added to pLineGroup afterwards.
        auto* colSep = new SoSeparator();
        pLineGroup->addChild(colSep);

        auto* off = new SoTransform();
        if (!offs.empty()) {
            const Base::Vector3d& v = offs[0];
            off->translation.setValue(
                (float)v.x, (float)v.y, (float)v.z);
        }
        colSep->addChild(off);

        auto* lineRot = new SoTransform();
        lineRot->rotation.setValue(SbVec3f(0.0f, 0.0f, 1.0f), negRot);
        colSep->addChild(lineRot);

        auto* scale = new SoTransform();
        m_lineScales.push_back(scale);
        colSep->addChild(scale);

        auto* shear = new SoMatrixTransform();
        m_lineShears.push_back(shear);
        colSep->addChild(shear);

        // Vertical text with a CAD shape font is stroked with its native
        // glyphs, one under the other; otherwise keep the multi-line
        // SoAsciiText path (which also centres/right-aligns per attachment).
        if (addShxColumn(lines, colSep)) {
            // stroked path added its line nodes above
        }
        else if (!probeSkipTtfNodes()) {
            auto* label = new SoAsciiText();
            label->string.setNum(static_cast<int>(lines.size()));
            for (size_t i = 0; i < lines.size(); ++i) {
                const std::string& line = lines[i];
                label->string.set1Value(
                    static_cast<int>(i),
                    SbString(line.empty() ? " " : line.c_str()));
            }

            int ap = anno->Attachment.getValue();
            if (ap < 1 || ap > 9) {
                ap = 1;
            }
            const int col = (ap - 1) % 3;  // 0 left, 1 center, 2 right
            if (col == 1) {
                label->justification.setValue(SoAsciiText::CENTER);
            }
            else if (col == 2) {
                label->justification.setValue(SoAsciiText::RIGHT);
            }
            else {
                label->justification.setValue(SoAsciiText::LEFT);
            }

            const double fh = anno->FontHeight.getValue();
            if (fh > 0.0) {
                const double lsFactor = anno->LineSpacing.getValue() > 0.0
                    ? anno->LineSpacing.getValue() : 1.0;
                label->spacing.setValue(static_cast<float>(lsFactor));
            }
            pLineGroup->addChild(label);
        }
    }
    else {
        for (size_t i = 0; i < lines.size(); ++i) {
            // One separator per line.  In Coin a transform is state that keeps
            // applying to the FOLLOWING siblings of its parent, so adding
            // off/scale/shear/label straight to pLineGroup made every line
            // inherit the translations of all the lines before it: row i was
            // drawn at the SUM of the offsets instead of its own.  Measured on
            // the wrapped road labels (MTEXT 西渚路, LabelOffsets -6 / -15.96 /
            // -25.92, height 6): the drawn row pitch came out 2.9x and 4.8x the
            // glyph height where the layout asks for 1.93x, exactly the sums
            // (-21.96 / -47.88).  Single-line text never showed it (one offset,
            // nothing to accumulate) and vertical text reads offs[0] only, so
            // it went unnoticed until wrapping produced multi-line MTEXT.
            auto* lineSep = new SoSeparator();
            pLineGroup->addChild(lineSep);

            auto* off = new SoTransform();
            if (i < offs.size()) {
                const Base::Vector3d& v = offs[i];
                off->translation.setValue(
                    (float)v.x, (float)v.y, (float)v.z);
            }
            lineSep->addChild(off);

            auto* scale = new SoTransform();
            m_lineScales.push_back(scale);
            lineSep->addChild(scale);

            auto* shear = new SoMatrixTransform();
            m_lineShears.push_back(shear);
            lineSep->addChild(shear);

            // AutoCAD single-line fonts (txt.shx, gbcbig.shx, ...) are
            // rendered from their native strokes so the glyphs match CAD;
            // TrueType text keeps the Coin text node.
            const std::string& line = lines[i];
            if (addShxLine(line, off, lineSep)) {
                continue;
            }
            if (probeSkipTtfNodes()) {
                continue;  // probe: no SoAsciiText node for TrueType text
            }
            auto* label = new SoAsciiText();
            label->string.setNum(1);
            label->string.set1Value(
                0, SbString(line.empty() ? " " : line.c_str()));
            lineSep->addChild(label);
        }
    }
    updateScale();
    updateShear();
}

bool ViewProviderDWGText::addShxLine(const std::string& line,
                                     SoTransform* lineOff, SoGroup* group)
{
    auto* anno = dynamic_cast<DWGText*>(pcObject);
    if (!anno) {
        return false;
    }
    if (line.empty()) {
        return false;
    }
    const std::string& fontFile = anno->FontFile.getValue();
    const std::string& bigFontFile = anno->BigFontFile.getValue();
    // Only classic AutoCAD shape fonts (.shx) can be stroked by
    // renderShxLine.  TrueType / OpenType files (simsun.ttf, arial_0.ttf, ...)
    // must stay on the SoAsciiText path - tryShx is not attempted for them.
    const bool fontIsShx = isShxFontName(fontFile);
    const bool bigIsShx = isShxFontName(bigFontFile);
    if (!fontIsShx && !bigIsShx) {
        SWDWG_LOG(
            "[SWDWG VP] '%s': TTF font (font='%s' bigfont='%s'), TTF path\n",
            anno->Label.getValue(), fontFile.c_str(),
            bigFontFile.c_str());
        return false;  // TrueType / bare family: SoAsciiText renders it
    }
    // A .shx style that exists locally can be drawn; otherwise the font is a
    // TrueType file / bare family and SoAsciiText handles it.
    const double fh = anno->FontHeight.getValue();
    const double wf = anno->WidthFactor.getValue() > 0.0f
        ? anno->WidthFactor.getValue() : 1.0f;
    const int cp = anno->Codepage.getValue();

    SWDWG_LOG(
        "[SWDWG VP] '%s': try SHX font='%s' bigfont='%s' cp=%d h=%.3f "
        "line='%s'\n",
        anno->Label.getValue(), fontFile.c_str(), bigFontFile.c_str(), cp,
        fh, line.c_str());

    const auto shxT0 = std::chrono::steady_clock::now();
    ShxLineRender r = renderShxLine(fontFile, bigFontFile, cp, line, fh, wf);
    g_vpShxSec += std::chrono::duration<double>(std::chrono::steady_clock::now()
                                                - shxT0)
                      .count();
    if (!r.ok || r.glyphs.empty()) {
        SWDWG_LOG(
            "[SWDWG VP] '%s': renderShxLine failed (ok=%d glyphs=%zu) - "
            "TTF fallback\n",
            anno->Label.getValue(), (int)r.ok, r.glyphs.size());
        return false;
    }

    // Single-line TEXT: re-place the line from the ACTUAL stroked geometry.
    //
    // renderShxLine draws big-font (CJK) glyphs FROM the baseline upward:
    // the run origin y=0 IS the AutoCAD baseline and the strokes span
    // roughly [0, h] (AutoCAD big-font cells sit on the baseline - verified
    // against a Justify=center/baseline fsdb.shx TEXT where the glyphs came
    // out h/2 above the LIST centre point until the rendering was shifted).
    // DWGText::recomputeLayout() on the other hand computed LabelOffsets
    // with TrueType metrics and adds a TTF-baseline vertical shift (dy =
    // -0.35h for middle, +0.25h bottom, -0.7h top); applying that on top of
    // SHX glyphs double-shifts the line.  For single-line TEXT we therefore
    // override the placement here from the real stroked extent:
    //    horizontal  left/center/right -> shift of the run origin;
    //    vertical    baseline(0)  -> 0 (glyphs already sit on the anchor);
    //                bottom(1)    -> align stroke-box bottom with the anchor;
    //                middle(2)    -> align stroke-box centre with the anchor;
    //                top(3)       -> align stroke-box top with the anchor.
    // Aligned(3) / Fit(5) are skipped: their layout (rotation + Fit height)
    // was already resolved by DWGText from AlignPt2 and the offsets stay 0.
    // MTEXT / vertical TEXT keep the block layout computed by the importer.
    if (lineOff && !anno->IsMText.getValue()
        && !anno->IsVertical.getValue()) {
        const int ha = anno->HorAlign.getValue();
        const int va = anno->VertAlign.getValue();
        if (ha != 3 && ha != 5) {
            double dx = 0.0;
            double dy = 0.0;
            if (ha == 1 || ha == 4) {  // center / middle: centre on anchor
                dx = -0.5 * r.width;
            }
            else if (ha == 2) {        // right: right edge on anchor
                dx = -r.width;
            }
            if (va == 1) {             // bottom: box bottom on the anchor
                dy = -r.descender;
            }
            else if (va == 2) {        // middle: box centre on the anchor
                dy = -0.5 * (r.ascender + r.descender);
            }
            else if (va == 3) {        // top: box top on the anchor
                dy = -r.ascender;
            }
            // va 0 (baseline): glyphs already sit on the anchor baseline.
            lineOff->translation.setValue((float)dx, (float)dy, 0.0f);
            const Base::Vector3d p = anno->Position.getValue();
            SWDWG_LOG(
                "[SWDWG VP] '%s': TEXT align ha=%d va=%d pos=(%.4f,%.4f,%.4f)"
                " w=%.4f asc=%.4f desc=%.4f -> dx=%.4f dy=%.4f\n",
                anno->Label.getValue(), ha, va, p.x, p.y, p.z, r.width,
                r.ascender, r.descender, dx, dy);
        }
    }

    // Feed every stroke segment into one SoCoordinate3 + SoIndexedLineSet.
    std::vector<SbVec3f> pts;
    std::vector<int32_t> idx;
    pts.reserve(r.glyphs.size() * 8);
    idx.reserve(r.glyphs.size() * 16);
    for (const auto& glyph : r.glyphs) {
        appendGlyphSegs(glyph, 0.0, 0.0, pts, idx);
    }
    if (pts.empty()) {
        return false;
    }
    addLineNodes(group, pts, idx);
    return true;
}

bool ViewProviderDWGText::addShxColumn(
    const std::vector<std::string>& chars, SoGroup* group)
{
    auto* anno = dynamic_cast<DWGText*>(pcObject);
    if (!anno || chars.empty()) {
        return false;
    }
    const std::string& fontFile = anno->FontFile.getValue();
    const std::string& bigFontFile = anno->BigFontFile.getValue();
    const bool fontIsShx = isShxFontName(fontFile);
    const bool bigIsShx = isShxFontName(bigFontFile);
    if (!fontIsShx && !bigIsShx) {
        return false;  // TrueType: keep the multi-line SoAsciiText path
    }
    const double fh = anno->FontHeight.getValue();
    const double wf = anno->WidthFactor.getValue() > 0.0f
        ? anno->WidthFactor.getValue() : 1.0f;
    const int cp = anno->Codepage.getValue();
    // Match the spacing the SoAsciiText path uses (line spacing factor x the
    // text height; the layout code already applied the same product).
    const double lsFactor = anno->LineSpacing.getValue() > 0.0f
        ? anno->LineSpacing.getValue() : 1.0f;
    const double step = fh * lsFactor;

    // Vertical text in a shape font: one glyph per character, drawn from top
    // to bottom.  We render every character through renderShxLine (it yields
    // the strokes of a single glyph with its advance/centring applied) and
    // stack the glyphs along -Y.
    std::vector<SbVec3f> pts;
    std::vector<int32_t> idx;
    pts.reserve(chars.size() * 12);
    idx.reserve(chars.size() * 24);

    for (size_t i = 0; i < chars.size(); ++i) {
        const std::string& ch = chars[i];
        if (ch.empty()) {
            continue;
        }
        ShxLineRender r =
            renderShxLine(fontFile, bigFontFile, cp, ch, fh, wf);
        if (!r.ok || r.glyphs.empty()) {
            continue;  // whitespace / unmapped character: keep the step
        }
        // Each renderShxLine output starts at x=0 with the glyph baseline on
        // y=0.  The i-th character belongs one text height below the first.
        const double dy = -static_cast<double>(i) * step;
        for (const auto& glyph : r.glyphs) {
            appendGlyphSegs(glyph, 0.0, dy, pts, idx);
        }
    }
    if (pts.empty()) {
        return false;
    }
    addLineNodes(group, pts, idx);
    return true;
}

void ViewProviderDWGText::updateScale()
{
    auto* anno = dynamic_cast<DWGText*>(pcObject);
    if (!anno) {
        return;
    }
    const float wf = anno->WidthFactor.getValue() > 0.0f
        ? anno->WidthFactor.getValue() : 1.0f;
    const int mirror = anno->Mirror.getValue();
    const float mSx = (mirror & 2) ? -1.0f : 1.0f;
    const float mSy = (mirror & 1) ? -1.0f : 1.0f;
    // The .shx engine already scales every stroke AND the advance by
    // WidthFactor while rasterising the glyphs (addShxLine).  Applying wf in
    // this node as well would square it (wf=0.75 -> 0.5625) and made letters
    // crowd into each other when the user lowered wf.  Only mirror signs are
    // applied here for .shx rows; TrueType rows keep the wf scaling here
    // because SoAsciiText has no internal width factor.
    const float sx = engineIsShx(anno) ? mSx : wf * mSx;
    const float sy = mSy;
    for (SoTransform* scale : m_lineScales) {
        scale->scaleFactor.setValue(sx, sy, 1.0f);
    }
}

void ViewProviderDWGText::updateShear()
{
    auto* anno = dynamic_cast<DWGText*>(pcObject);
    if (!anno) {
        return;
    }
    const double obl = anno->ObliqueAngle.getValue();  // radians
    SbMatrix m;
    m.makeIdentity();
    if (std::fabs(std::tan(obl)) > 1e-9) {
        // Column-major: x' = x + tan(oblique) * y.
        m[1][0] = static_cast<float>(std::tan(obl));
    }
    for (SoMatrixTransform* shear : m_lineShears) {
        shear->matrix.setValue(m);
    }
}

void ViewProviderDWGText::updateData(const App::Property* prop)
{
    // While the importer is filling a freshly added object the properties are
    // written one by one.  Rebuilding the glyph scene graph per property
    // (each rebuild runs the SHX decoding / FreeType pipeline for every line)
    // makes importing large drawings quadratic in the text count.  Defer all
    // rebuilds to the final property write of the bulk-create window; the
    // importer then runs recomputeLayoutOnce() which re-writes LabelOffsets
    // and triggers this single updateData with the window closed.
    // A .fcstd restore writes each property back the same way (no bulk window
    // is open), so the same deferral is applied there; finishRestoring()
    // performs the single rebuild once every property is restored.
    const bool bulk = deferHeavyRebuild();

    if (prop && strcmp(prop->getName(), "LabelText") == 0) {
        if (!bulk) {
            rebuildLines();
        }
    }
    else if (prop && strcmp(prop->getName(), "LabelOffsets") == 0) {
        if (!bulk) {
            rebuildLines();
        }
    }
    else if (prop && prop->is<App::PropertyVector>()
             && strcmp(prop->getName(), "Position") == 0) {
        Base::Vector3d v = static_cast<const App::PropertyVector*>(prop)->getValue();
        pTransform->translation.setValue((float)v.x, (float)v.y, (float)v.z);
    }
    else if (prop && strcmp(prop->getName(), "FontHeight") == 0) {
        pFont->size = static_cast<const App::PropertyFloat*>(prop)->getValue();
        // .shx strokes are rasterised at the text height; a height change
        // must re-run the SHX decode (the pFont->size above only affects the
        // SoAsciiText / TrueType path).
        if (!bulk && engineIsShx(dynamic_cast<DWGText*>(pcObject))) {
            rebuildLines();
        }
    }
    else if (prop && strcmp(prop->getName(), "Rotation") == 0) {
        double angle = static_cast<const App::PropertyAngle*>(prop)->getValue();
        pTransform->rotation.setValue(SbVec3f(0.0f, 0.0f, 1.0f), (float)angle);
        auto* anno = dynamic_cast<DWGText*>(pcObject);
        if (anno && anno->IsVertical.getValue() && !bulk) {
            rebuildLines();
        }
    }
    else if (prop && strcmp(prop->getName(), "TextColor") == 0) {
        const Base::Color& c =
            static_cast<const App::PropertyColor*>(prop)->getValue();
        pColor->rgb.setValue(c.r, c.g, c.b);
    }
    else if (prop && strcmp(prop->getName(), "FontName") == 0) {
        pFont->name = static_cast<const App::PropertyString*>(prop)->getValue();
    }
    else if (prop && strcmp(prop->getName(), "WidthFactor") == 0) {
        if (!bulk) {
            if (engineIsShx(dynamic_cast<DWGText*>(pcObject))) {
                // WidthFactor is baked into the .shx stroke coordinates and
                // the inter-character advance; re-rasterise the glyphs.
                rebuildLines();
            }
            else {
                updateScale();
            }
        }
    }
    else if (prop && strcmp(prop->getName(), "Mirror") == 0) {
        if (!bulk) {
            updateScale();
        }
    }
    else if (prop && strcmp(prop->getName(), "ObliqueAngle") == 0) {
        if (!bulk) {
            updateShear();
        }
    }
    // Font engine selection (SHX vs TTF) depends on these; a change must
    // rebuild the line nodes or a text created before its STYLE font was
    // populated would stay stuck on the SoAsciiText (TTF) path.
    else if (prop && (strcmp(prop->getName(), "FontFile") == 0
                      || strcmp(prop->getName(), "BigFontFile") == 0
                      || strcmp(prop->getName(), "Codepage") == 0
                      || strcmp(prop->getName(), "RenderEngine") == 0)) {
        if (!bulk) {
            rebuildLines();
        }
    }
    else if (prop && strcmp(prop->getName(), "IsVertical") == 0) {
        if (!bulk) {
            rebuildLines();
        }
    }

    // Invalidate the cached display list so the modified glyphs are rebuilt.
    if (pRoot) {
        pRoot->touch();
    }

    ViewProviderDocumentObject::updateData(prop);
}

std::vector<std::string> ViewProviderDWGText::getDisplayModes() const
{
    return {"3D"};
}

void ViewProviderDWGText::setDisplayMode(const char* ModeName)
{
    if (strcmp(ModeName, "3D") == 0) {
        setDisplayMaskMode("3D");
    }
    ViewProviderDocumentObject::setDisplayMode(ModeName);
}

void ViewProviderDWGText::onChanged(const App::Property* prop)
{
    ViewProviderDocumentObject::onChanged(prop);
}
