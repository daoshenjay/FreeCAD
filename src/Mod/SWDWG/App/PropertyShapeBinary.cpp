#include "PreCompiled.h"

#include "PropertyShapeBinary.h"

// Base::Writer must be complete: the class switches its mode and forwards the
// call to the base class implementation.
#include <Base/Writer.h>

using namespace SWDWG;

TYPESYSTEM_SOURCE(SWDWG::PropertyShapeBinary, Part::PropertyPartShape)

namespace
{

//! RAII: force Base::Writer into its binary-BRep mode for the current call and
//! restore whatever mode the document was using before.
class BinaryBrepMode
{
public:
    explicit BinaryBrepMode(Base::Writer& writer)
        : m_writer(writer)
        , m_previous(writer.getMode("BinaryBrep"))
    {
        if (!m_previous) {
            m_writer.setMode("BinaryBrep");
        }
    }
    ~BinaryBrepMode()
    {
        if (!m_previous) {
            m_writer.clearMode("BinaryBrep");
        }
    }
    BinaryBrepMode(const BinaryBrepMode&) = delete;
    BinaryBrepMode& operator=(const BinaryBrepMode&) = delete;

private:
    Base::Writer& m_writer;
    bool m_previous;
};

}  // namespace

void PropertyShapeBinary::Save(Base::Writer& writer) const
{
    BinaryBrepMode mode(writer);
    Part::PropertyPartShape::Save(writer);
}

void PropertyShapeBinary::SaveDocFile(Base::Writer& writer) const
{
    BinaryBrepMode mode(writer);
    Part::PropertyPartShape::SaveDocFile(writer);
}

App::Property* PropertyShapeBinary::Copy() const
{
    auto* prop = new PropertyShapeBinary();
    prop->setValue(getValue());
    return prop;
}

void PropertyShapeBinary::Paste(const App::Property& from)
{
    const auto& source = dynamic_cast<const PropertyShapeBinary&>(from);
    setValue(source.getValue());
}
