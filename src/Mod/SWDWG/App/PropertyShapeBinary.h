//! Persistence-only variant of Part::PropertyPartShape that always stores the
//! geometry as binary BRep inside the .FCStd.
//!
//! The object-level semantics are unchanged (getValue()/setValue() still deal
//! in TopoDS_Shape, the Python attribute is still a Shape), only the on-disk
//! format changes from ASCII ".brp" to binary ".bin" - the same idea as
//! SWDTM::PropertyIndexList (see SWDTM/doc/DTM_LIBRARY_NOTES.md §15).
//!
//! Measured on 地形图.dwg (1,406,427 edges, 644 MB of ASCII BRep):
//!   ASCII  .brp : save 54-67 s, file 124.6 MB, open 21.5 s
//!   binary .bin : save 23.8 s , file  91.7 MB, open 10.1 s
//!
//! The implementation deliberately does not duplicate the base class' save
//! code: Save()/SaveDocFile() just switch the writer into its "BinaryBrep"
//! mode for the duration of the call, so the base class takes its own binary
//! branch, and restore the previous mode afterwards (other properties written
//! into the same document are unaffected).
//!
//! Compatibility: the binary entry is read back by the inherited
//! Restore/RestoreDocFile (they pick importBinary() by the ".bin" extension).
//! The *property type name* changes though, and PropertyContainer::Restore
//! compares type names strictly - so older documents silently skip this
//! property.  DWGLineGroup therefore refills it in onDocumentRestored() from
//! whatever geometry the old file did carry (see there), which keeps every
//! older file working.

#pragma once

#include <Mod/Part/App/PropertyTopoShape.h>

#include "../SWDWGGlobal.h"

namespace SWDWG
{

class SWDWGExport PropertyShapeBinary: public Part::PropertyPartShape
{
    TYPESYSTEM_HEADER_WITH_OVERRIDE();

public:
    PropertyShapeBinary() = default;
    ~PropertyShapeBinary() override = default;

    /// Write the <Part ... file="...bin"/> reference (base class logic, binary
    /// mode forced).
    void Save(Base::Writer& writer) const override;
    /// Write the geometry itself as binary BRep (base class logic, binary mode
    /// forced).
    void SaveDocFile(Base::Writer& writer) const override;

    App::Property* Copy() const override;
    void Paste(const App::Property& from) override;
};

}  // namespace SWDWG
