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

#ifndef SWDWG_DWGREADER_H
#define SWDWG_DWGREADER_H

#include <map>
#include <string>

// libredwg C API
extern "C" {
#include "dwg.h"
}

// PyCXX
#include <CXX/Objects.hxx>

namespace SWDWG
{

/**
 * DWGReader wraps the LibreDWG C API to read a .dwg file and build a
 * Python dictionary describing the file contents:
 *
 *   {
 *     "filename": ...,
 *     "version":  ...,
 *     "layers":   [ {"name":..,"color":..,"off":..,"frozen":..,"locked":..,
 *                     "linetype":..}, ... ],
 *     "styles":   [ {"name":..,"height":..,"width_factor":..,"oblique":..,
 *                     "font":..}, ... ],
 *     "dimstyles":[ {"name":..}, ... ],
 *     "linetypes":[ {"name":..,"description":..,"pattern_len":..,
 *                     "dashes":[...]}, ... ],
 *     "blocks":   [ {"name":..,"base_point":[x,y,z],"insert_units":..}, ... ],
 *     "entities": [ {"type":..,"layer":..,"color":..,"rgb":..,
 *                    "geometry": {...}}, ... ]
 *   }
 */
class DWGReader
{
public:
    DWGReader();
    ~DWGReader();

    //! Try to open a DWG file and extract its contents.
    //! Returns true on success. On failure `error` holds the message.
    bool read(const std::string& filename, std::string& error);

    //! Returns the extracted data as a Python dictionary.
    Py::Dict result() const { return m_result; }

private:
    // internal helpers (each fills m_result)
    void readHeader();
    void readLayers();
    void readStyles();
    void readDimstyles();
    void readLinetypes();
    void readBlocks();
    void readEntities();
    //! Per-layer / per-type count of *every* entity object in the file,
    //! independent of whether a geometry handler exists for it.  readEntities()
    //! can only report the types DWGReader knows how to describe, which makes a
    //! layer holding nothing but HATCH or old-style POLYLINE entities look
    //! empty; this inventory is what answers "what is actually on layer X" when
    //! something goes missing after import.
    void readInventory();

    // Convert a raw byte string from LibreDWG into a Python str.
    // DWG text is UTF-8 from R2007 on, but older files may use a legacy
    // single-byte code page. Try strict UTF-8 first and fall back to
    // Latin-1 so that no byte is ever lost and decoding never throws.
    static Py::String str(const char* s);

    // entity geometry extraction (returns a Py::Dict or Py::None)
    Py::Object entityGeometry(const Dwg_Object* obj);

    // entity common attributes -> dict
    Py::Dict entityCommon(const Dwg_Object* obj);

    // build a map of entity object -> owning block name
    void buildEntityBlockMap();

    Dwg_Data* m_dwg;
    size_t    m_objectCount;
    Py::Dict  m_result;
    std::map<const Dwg_Object*, std::string> m_entityBlock;
};

} // namespace SWDWG

#endif // SWDWG_DWGREADER_H
