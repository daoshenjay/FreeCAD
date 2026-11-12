#include "PreCompiled.h"

#include "DWGText.h"

#include "DWGInternal.h"
#include "FTTextRenderer.h"
#include "TextShxRenderer.h"

#include <Base/Console.h>

#include <algorithm>
#include <sstream>

using namespace SWDWG;

namespace
{
const double PI = 3.14159265358979323846;

//! Depth of the "bulk text creation" windows opened by the importer.  While
//! non-zero, onChanged() does not re-run the (expensive) layout and the Gui
//! view provider defers glyph rebuilding until the window closes.
int& bulkCreateDepth()
{
    static int depth = 0;
    return depth;
}

//! Rough single-line width estimate shared by the importer and the runtime
//! recompute: ASCII ~0.6 em, CJK/fullwidth ~1.0 em.
double estimateTextWidth(const std::string& text, double fontHeight)
{
    size_t ascii = 0, wide = 0;
    for (size_t i = 0; i < text.size();) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        size_t extra = 0;
        if ((c & 0x80) == 0) {
            extra = 0;
        }
        else if ((c & 0xE0) == 0xC0) {
            extra = 1;
        }
        else if ((c & 0xF0) == 0xE0) {
            extra = 2;
        }
        else if ((c & 0xF8) == 0xF0) {
            extra = 3;
        }
        else {
            ++i;
            continue;
        }
        if (extra >= 2) {
            ++wide;
        }
        else {
            ++ascii;
        }
        i += extra + 1;
    }
    return fontHeight * (ascii * 0.6 + wide * 1.0);
}

//! Measure a text line with the current object's TrueType family
//! (FontName), falling back to the rough estimate when FreeType cannot
//! resolve the family.  The width factor is applied inside the measurement
//! so a centred / right-aligned run lines up with the drawn glyphs.
double measureWidthWithFont(const DWGText& t, const std::string& text,
                            double fontHeight)
{
    if (text.empty() || fontHeight <= 0.0) {
        return 0.0;
    }
    const std::string& family = t.FontName.getValue();
    const double wf = t.WidthFactor.getValue() > 0.0f
        ? t.WidthFactor.getValue() : 1.0f;
    const double w = measureTextWidth(family, text, fontHeight, wf);
    return w >= 0.0 ? w : estimateTextWidth(text, fontHeight) * wf;
}

}  // namespace

PROPERTY_SOURCE(SWDWG::DWGText, App::Annotation)

DWGText::DWGText()
{
    ADD_PROPERTY_TYPE(FontHeight,
                      (2.5),
                      "DWG",
                      App::Prop_None,
                      "Text height in world units (mm)");
    ADD_PROPERTY_TYPE(Rotation,
                      (0.0),
                      "DWG",
                      App::Prop_None,
                      "Rotation about the Z axis (radians)");
    ADD_PROPERTY_TYPE(TextColor,
                      (1.0f, 1.0f, 1.0f),
                      "DWG",
                      App::Prop_None,
                      "Text color (resolved from the DWG entity/layer)");
    ADD_PROPERTY_TYPE(FontName,
                      ("Arial"),
                      "DWG",
                      App::Prop_None,
                      "TrueType font name used for rendering");
    ADD_PROPERTY_TYPE(Layer,
                      ("0"),
                      "DWG",
                      App::Prop_None,
                      "DWG layer name the entity lives on");
    ADD_PROPERTY_TYPE(EntityKind,
                      (""),
                      "DWG",
                      App::Prop_ReadOnly,
                      "Imported entity kind: TEXT or MTEXT (tree grouping key)");
    ADD_PROPERTY_TYPE(StyleName,
                      (""),
                      "DWG",
                      App::Prop_None,
                      "STYLE table entry the entity references");
    ADD_PROPERTY_TYPE(WidthFactor,
                      (1.0f),
                      "DWG",
                      App::Prop_None,
                      "Horizontal stretch factor (width/height ratio)");
    ADD_PROPERTY_TYPE(ObliqueAngle,
                      (0.0),
                      "DWG",
                      App::Prop_None,
                      "Oblique angle (radians; AutoCAD stores degrees)");
    ADD_PROPERTY_TYPE(Mirror,
                      (0),
                      "DWG",
                      App::Prop_None,
                      "Text generation flags: 0 normal, 1 upside-down, 2 backward");
    static const char* hAlignEnums[] = {"Left", "Center", "Right",
                                        "Aligned", "Middle", "Fit", nullptr};
    static const char* vAlignEnums[] = {"Baseline", "Bottom", "Middle",
                                        "Top", nullptr};
    ADD_PROPERTY_TYPE(HorAlign,
                      (App::Enumeration("Left")),
                      "DWG",
                      App::Prop_None,
                      "TEXT horizontal alignment: 0 left, 1 center, 2 right, "
                      "3 aligned, 4 middle, 5 fit");
    HorAlign.setEnums(hAlignEnums);
    HorAlign.setValue(0L);
    ADD_PROPERTY_TYPE(VertAlign,
                      (App::Enumeration("Baseline")),
                      "DWG",
                      App::Prop_None,
                      "TEXT vertical alignment: 0 baseline, 1 bottom, 2 middle, 3 top");
    VertAlign.setEnums(vAlignEnums);
    VertAlign.setValue(0L);
    ADD_PROPERTY_TYPE(Attachment,
                      (1),
                      "DWG",
                      App::Prop_None,
                      "MTEXT attachment point: 1 top-left .. 9 bottom-right");
    ADD_PROPERTY_TYPE(LineSpacing,
                      (1.0f),
                      "DWG",
                      App::Prop_None,
                      "MTEXT line spacing factor (0.25 .. 4.0)");
    ADD_PROPERTY_TYPE(LineSpacingStyle,
                      (1),
                      "DWG",
                      App::Prop_None,
                      "MTEXT line spacing style: 1 = at least, 2 = exactly");
    ADD_PROPERTY_TYPE(AlignPt2,
                      (Base::Vector3d(0.0, 0.0, 0.0)),
                      "DWG",
                      App::Prop_None,
                      "TEXT Aligned/Fit second baseline point (world units)");
    ADD_PROPERTY_TYPE(IsMText,
                      (false),
                      "DWG",
                      App::Prop_None,
                      "True for DWG MTEXT entities (per-line layout path)");
    ADD_PROPERTY_TYPE(IsVertical,
                      (false),
                      "DWG",
                      App::Prop_None,
                      "True when the text style has vertical direction");
    ADD_PROPERTY_TYPE(LabelOffsets,
                      (),
                      "DWG",
                      App::Prop_None,
                      "Per-line local offsets relative to the anchor point");
    ADD_PROPERTY_TYPE(FontFile,
                      (""),
                      "DWG",
                      App::Prop_None,
                      "STYLE font file name (e.g. txt.shx)");
    ADD_PROPERTY_TYPE(BigFontFile,
                      (""),
                      "DWG",
                      App::Prop_None,
                      "STYLE big-font (CJK) file name (e.g. fsdb.shx)");
    // ===================== 导出功能 BEGIN =====================
    // 原始插入点（DWG 的 DXF 组码 10）。TEXT 设了对齐方式时 CAD 按"对齐点"(组码 11)
    // 摆放，而组码 10 仍是文字框左下角的基线点；导出时按左/基线 + 这个点写，显示
    // 位置与源图完全一致，不用再靠字宽估算。
    ADD_PROPERTY_TYPE(InsPt,
                      (Base::Vector3d(0.0, 0.0, 0.0)),
                      "DWG",
                      App::Prop_None,
                      "原始插入点 (DXF 10)，导出时按左/基线写回");
    // ===================== 导出功能 END =======================
    ADD_PROPERTY_TYPE(Codepage,
                      (0),
                      "DWG",
                      App::Prop_None,
                      "DWG codepage of the drawing (e.g. 936 = GBK)");
    ADD_PROPERTY_TYPE(RenderEngine,
                      (0),
                      "DWG",
                      App::Prop_None,
                      "Rendering engine: 0 auto, 1 force TTF, 2 force SHX");
}

DWGText::~DWGText() = default;

void DWGText::enterBulkCreate()
{
    ++bulkCreateDepth();
}

void DWGText::leaveBulkCreate()
{
    if (bulkCreateDepth() > 0) {
        --bulkCreateDepth();
    }
}

bool DWGText::isBulkCreating()
{
    return bulkCreateDepth() > 0;
}

void DWGText::recomputeLayoutOnce()
{
    if (m_bRecomputing) {
        return;  // already inside recomputeLayout (guarded call)
    }
    m_bRecomputing = true;
    recomputeLayout();
    m_bRecomputing = false;
}

void DWGText::recomputeLayout()
{
    std::vector<std::string> lines = LabelText.getValues();
    if (lines.empty()) {
        LabelOffsets.setValues(std::vector<Base::Vector3d>());
        return;
    }
    const double fontHeight0 = FontHeight.getValue();
    double fontHeight = fontHeight0;
    double rotation = Rotation.getValue();
    const double wf = WidthFactor.getValue() > 0.0f
        ? WidthFactor.getValue() : 1.0f;
    const Base::Vector3d anchor = Position.getValue();
    const Base::Vector3d pt2 = AlignPt2.getValue();
    std::vector<Base::Vector3d> offs;

    if (!IsMText.getValue()) {
        // --- TEXT: single line -------------------------------------------
        const bool hasAlignPt2 = (pt2.x != 0.0 || pt2.y != 0.0
                                  || pt2.z != 0.0);
        if (hasAlignPt2) {
            // Aligned(3) / Fit(5): both points define the baseline direction
            // and extent; the anchor (Position) is the first point.
            const double dx2 = pt2.x - anchor.x;
            const double dy2 = pt2.y - anchor.y;
            const double len = std::sqrt(dx2 * dx2 + dy2 * dy2);
            if (len > 1e-9) {
                rotation = std::atan2(dy2, dx2);
                if (dx2 < 0.0) {
                    rotation += PI;  // AutoCAD reverses the text
                }
                if (HorAlign.getValue() == 5) {  // Fit: fill the span
                    const double w = measureWidthWithFont(
                        *this, lines[0], fontHeight);
                    if (w > 1e-9) {
                        fontHeight = fontHeight * len / w;
                    }
                }
                Rotation.setValue(rotation);
            }
        }
        else {
            const int hAlign = HorAlign.getValue();
            const int vAlign = VertAlign.getValue();
            double dx = 0.0;
            double dy = 0.0;
            // Width only shifts the line when the horizontal alignment is
            // centred / right (or the Middle special case); plain left text
            // does not need a measurement at all.
            const double w = (hAlign == 1 || hAlign == 2 || hAlign == 4)
                ? measureWidthWithFont(*this, lines[0], fontHeight)
                : 0.0;
            switch (hAlign) {
                case 1:  // center
                case 4:  // middle is horizontally centered as well
                    dx = -0.5 * w;
                    break;
                case 2:  // right
                    dx = -w;
                    break;
                default:
                    break;
            }
            switch (vAlign) {
                case 1:  // bottom (descender line): baseline above anchor
                    dy = 0.25 * fontHeight;
                    break;
                case 2:  // middle: anchor at bbox centre
                    dy = -0.35 * fontHeight;
                    break;
                case 3:  // top (cap height): baseline below anchor
                    dy = -0.7 * fontHeight;
                    break;
                default:
                    break;
            }
            if (hAlign == 4) {  // middle is vertically centered too
                dy = -0.35 * fontHeight;
            }
            offs.emplace_back(dx, dy, 0.0);
        }
        if (offs.empty()) {  // Aligned/Fit: left baseline sits on the anchor
            offs.emplace_back(0.0, 0.0, 0.0);
        }
    }
    else {
        // --- MTEXT: per-line layout --------------------------------------
        // Vertical text: render each character as a separate line, while
        // keeping the user-editable LabelText unchanged.
        const bool vertical = IsVertical.getValue();
        std::vector<std::string> layoutLines = lines;
        if (vertical && !lines.empty()) {
            std::string all;
            for (const std::string& ln : lines) {
                all += ln;
            }
            layoutLines = splitUtf8Chars(all);
        }
        const size_t rows = std::max<size_t>(layoutLines.size(), 1);
        const double lsFactor = LineSpacing.getValue() > 0.0f
            ? LineSpacing.getValue() : 1.0f;
        // Vertical text stacks characters with no extra 1.66 line gap.
        // 5/3 = AutoCAD's single line spacing, see kSingleLineSpacing in
        // DWGText.h (keep in step with Importer::computeTextLayout).
        double ls = vertical ? fontHeight * lsFactor
                             : fontHeight * kSingleLineSpacing * lsFactor;
        // Keep in step with Importer::computeTextLayout(): no clamp to single
        // spacing when the factor is below 1.  AutoCAD's own readout for a
        // 0.8x label is the plain product (5.3333 for a height of 4) and it
        // lays the rows out at that pitch.
        std::vector<double> widths(rows, 0.0);
        // Glyph box of each line relative to its baseline (y up).  The
        // fallback - fontHeight above the baseline, nothing below - matches a
        // stroked big-font cell, which sits on the baseline and is fontHeight
        // high (see the note in ViewProviderDWGText::addShxLine).
        std::vector<double> tops(rows, fontHeight);
        std::vector<double> bots(rows, 0.0);
        for (size_t i = 0; i < rows; ++i) {
            widths[i] = measureWidthWithFont(*this, layoutLines[i],
                                             fontHeight);
        }
        // A .shx style is stroked from the shape file by the view provider, so
        // place it with the metrics of those strokes.  The TrueType family
        // mapped from such a style measures quite differently: its advances
        // are scaled by face->height, which on this bridge plan's CJK styles
        // (fsdb_e.shx + fsdb.shx) works out to 1.5 em per fullwidth cell
        // against the 1.0 em cell the shape file really draws.  Centred MTEXT
        // was therefore placed with a box 1.5x too wide and came out a quarter
        // of its width off centre (reported as "part of the text is not
        // centred" for MTEXT 道路中线, attachment 正中, height 2.5).  Vertical
        // text keeps the font-based metrics: it is drawn as a single stroked
        // column whose placement is read from offs[0] alone.
        if (!vertical) {
            const std::string& ff = FontFile.getValue();
            const std::string& bf = BigFontFile.getValue();
            if (isShxFont(ff, bf)) {
                const int cp = Codepage.getValue();
                for (size_t i = 0; i < rows; ++i) {
                    const ShxLineRender r = renderShxLine(
                        ff, bf, cp, layoutLines[i], fontHeight, wf);
                    if (r.ok && r.width > 0.0) {
                        widths[i] = r.width;
                        tops[i] = r.ascender;
                        bots[i] = r.descender;
                    }
                }
            }
        }
        int ap = Attachment.getValue();
        if (ap < 1 || ap > 9) {
            ap = 1;
        }
        const int col = (ap - 1) % 3;  // 0 left, 1 center, 2 right
        const int row = (ap - 1) / 3;  // 0 top, 1 middle, 2 bottom
        // Place the whole BLOCK against the anchor, not just the lines inside
        // it.  The block spans from baseline_0 + tops[0] down to
        // baseline_{rows-1} + bots[rows-1].  The anchor is AutoCAD's
        // attachment point, so a centred / right / bottom attachment has to
        // move the whole block; applying only the differences between lines
        // (as this used to) left every such run half a block - or a full
        // block - right of and above its anchor.  Reported on the bridge
        // plan: MTEXT "道路中线" handle ba37, attachment 正中 (5), height 2.5,
        // which came out half a text width right of its anchor
        // (34071.8046, 46955.7025).  Column 0 and row 0 are unaffected: their
        // anchor already is the block corner the offsets describe.
        const double span = (rows > 0) ? static_cast<double>(rows - 1) * ls
                                       : 0.0;
        const double top0 = tops.empty() ? fontHeight : tops[0];
        const double botN = bots.empty() ? 0.0 : bots[rows - 1];
        const double base0 = (row == 0) ? -top0
            : (row == 1) ? 0.5 * (span - top0 - botN)
            : span - botN;
        for (size_t i = 0; i < rows; ++i) {
            double x = 0.0;
            if (col == 1) {
                x = -0.5 * widths[i];
            }
            else if (col == 2) {
                x = -widths[i];
            }
            const double y = base0 - static_cast<double>(i) * ls;
            offs.emplace_back(x, y, 0.0);
        }
        if (vertical) {
            std::ostringstream oss;
            oss << "[SWDWG VTEXT] fh=" << fontHeight << " ls=" << ls
                << " lsFactor=" << lsFactor << " rows=" << rows
                << " ap=" << ap << " wf=" << wf
                << " (off[0] locates column start)";
            for (const auto& o : offs) {
                oss << " (" << o.x << "," << o.y << ")";
            }
            SWDWG_LOG("%s\n", oss.str().c_str());
        }
    }

    LabelOffsets.setValues(offs);
    if (fontHeight != fontHeight0) {
        FontHeight.setValue(fontHeight);  // Fit height correction
    }
}

void DWGText::onChanged(const App::Property* prop)
{
    // During a bulk-create window (importer filling a fresh object) skip the
    // automatic re-layout: every property write would otherwise trigger a
    // FreeType width measurement of the text, so importing a drawing with
    // thousands of text entities costs a full layout pass per property.  The
    // importer calls recomputeLayoutOnce() after the window closes.
    //
    // The same holds while the object is being restored from a .fcstd file:
    // FreeCAD writes each layout property back one by one (each write lands
    // here with a fresh property), and LabelOffsets was already persisted -
    // re-measuring / re-laying-out per property is pure waste.  The layout is
    // only (re)computed when a user edits the text afterwards.
    if (!m_bRecomputing && !isBulkCreating() && !isRestoring() && prop
        && (prop == &LabelText || prop == &FontHeight
            || prop == &WidthFactor || prop == &HorAlign
            || prop == &VertAlign || prop == &Attachment
            || prop == &LineSpacing || prop == &LineSpacingStyle
            || prop == &AlignPt2 || prop == &Position
            || prop == &IsMText || prop == &IsVertical)) {
        m_bRecomputing = true;
        recomputeLayout();
        m_bRecomputing = false;
    }
    App::Annotation::onChanged(prop);
}
