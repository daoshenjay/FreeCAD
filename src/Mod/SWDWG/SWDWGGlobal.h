// SPDX-License-Identifier: LGPL-2.1-or-later

/***************************************************************************
 *   Copyright (c) 2022 Werner Mayer <wmayer[at]users.sourceforge.net>     *
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

#include <FCGlobal.h>

#ifndef SWDWG_GLOBAL_H
# define SWDWG_GLOBAL_H


// SWDWG
# ifndef SWDWGExport
#  ifdef SWDWG_EXPORTS
#   define SWDWGExport FREECAD_DECL_EXPORT
#  else
#   define SWDWGExport FREECAD_DECL_IMPORT
#  endif
# endif

// ---------------------------------------------------------------------
// SWDWG release version.
//
// 1.0  = first released state of the DWG import module: line work with
//        layer/colour/line-weight/linetype fidelity (simple + complex
//        linetypes, L1 quantized pattern and L2 exact cutting),
//        TEXT/MTEXT with the .shx engine, DIMENSIONs, LEADERs, HATCHes,
//        3DFACE surfaces, blocks/inserts, the SWDTM terrain hand-off, and
//        the two-layer unit rule (see run() in DWGImporter.cpp).
//
// Printed at the head of every import so a pasted log identifies the build.
# define SWDWG_VERSION "1.0.0"

// SWDWGGui
# ifndef SWDWGGuiExport
#  ifdef SWDWGGui_EXPORTS
#   define SWDWGGuiExport FREECAD_DECL_EXPORT
#  else
#   define SWDWGGuiExport FREECAD_DECL_IMPORT
#  endif
# endif

// ---------------------------------------------------------------------
// SWDWG debug-log switch.
//
// Diagnostic output of the module (the "[SWDWG ...]" progress / tracing
// messages) is emitted ONLY when `sw_debug` is defined.  Enable it either by
// uncommenting the #define below or by adding /Dsw_debug to the SWDWG build.
// With `sw_debug` undefined every SWDWG_LOG(...) call compiles to nothing, so
// release builds print no module chatter.
//
// Errors and warnings (Base::Console().error / .warning) are NOT gated: they
// always stay visible.
// #define sw_debug

# if defined(sw_debug)
#  include <Base/Console.h>
#  define SWDWG_LOG(...) ::Base::Console().message(__VA_ARGS__)
# else
#  define SWDWG_LOG(...) ((void)0)
# endif

#endif  //SWDWG_GLOBAL_H
