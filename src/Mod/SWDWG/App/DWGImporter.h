// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2024 YOUR NAME <Your e-mail address>                    *
 *                                                                         *
 *   This file is part of the FreeCAD CAx development system.              *
 *                                                                         *
 *   This library is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU Library General Public           *
 *   License as published by the Free Software Foundation; either          *
 *   version 2 of the License, or (at your option) any later version.      *
 *                                                                         *
 *   This library  is distributed in the hope that it will be useful,      *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this library; see the file COPYING.LIB. If not,    *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,         *
 *   Suite 330, Boston, MA  02111-1307, USA                                *
 *                                                                         *
 ***************************************************************************/

#pragma once

#include <App/Document.h>

#include <functional>
#include <string>

#include "../SWDWGGlobal.h"

namespace SWDWG
{

//! Progress callback used by the GUI to drive a progress dialog while the
//! (potentially long) import runs.  \a fraction is 0..1, \a phase is a short
//! human-readable stage description.  Called on the calling (GUI) thread.
using ImportProgressFn = std::function<void(double fraction, const std::string& phase)>;

//! Pure C++ DWG importer.
//!
//! The complete import pipeline runs in native C++ and never touches the
//! Python wrapper layers: read the DWG with LibreDWG, expand INSERT blocks,
//! apply block transforms with plain double-precision matrix math, build OCC
//! shapes and create Part::Feature objects in a FreeCAD document.
class SWDWGExport DWGImporter
{
public:
    //! Import \a filename into \a doc.
    //! On success returns true and stores the object counts in
    //! \a created / \a skipped. On failure returns false and stores a human
    //! readable message in \a error.
    //! \a progress (optional) is invoked with (0..1 fraction, phase text) so
    //! a GUI can show a progress dialog; pass an empty callback when no UI is
    //! wanted (command-line / Python import).
    static bool importDWG(App::Document* doc, const std::string& filename,
                          std::string& error, int& created, int& skipped,
                          ImportProgressFn progress = {});
};

}  // namespace SWDWG
