# SPDX-License-Identifier: LGPL-2.1-or-later

# ---------------------------------------------------------------------------
# LibreDWG (optional but recommended)
#
# DWGReader.cpp links against LibreDWG to read .dwg files natively.
# Set SWDWG_LIBREDWG_DIR to the top-level libredwg source/build directory.
# ---------------------------------------------------------------------------
set(SWDWG_LIBREDWG_DIR "" CACHE PATH
    "Path to the LibreDWG directory containing include/dwg.h and the built library")

if(NOT SWDWG_LIBREDWG_DIR)
    # Fallback default relative to the SWDWG module location
    set(SWDWG_LIBREDWG_DIR "D:/libredwg-master/libredwg")
endif()

set(SWDWG_LIBREDWG_INCLUDE_DIR "")
set(SWDWG_LIBREDWG_LIB "")

if(EXISTS "${SWDWG_LIBREDWG_DIR}/include/dwg.h")
    set(SWDWG_LIBREDWG_INCLUDE_DIR "${SWDWG_LIBREDWG_DIR}/include")
endif()

# Locate the built static library
foreach(_candidate
        "${SWDWG_LIBREDWG_DIR}/lib/libredwg.lib"
        "${SWDWG_LIBREDWG_DIR}/src/.libs/libredwg.a"
        "${SWDWG_LIBREDWG_DIR}/src/libredwg.lib"
        "${SWDWG_LIBREDWG_DIR}/Release/libredwg.lib")
    if(EXISTS "${_candidate}")
        set(SWDWG_LIBREDWG_LIB "${_candidate}")
        break()
    endif()
endforeach()

set(SWDWG_LIBS
    FreeCADApp
    Part
    ${OCC_LIBRARIES}
    ${OCC_DEBUG_LIBRARIES}
)

if(SWDWG_LIBREDWG_INCLUDE_DIR AND SWDWG_LIBREDWG_LIB)
    message(STATUS "SWDWG: using LibreDWG at ${SWDWG_LIBREDWG_LIB}")
else()
    message(WARNING "SWDWG: LibreDWG library not found - DWG reading disabled")
endif()

SET(SWDWG_SRCS
    AppSWDWG.cpp
    DWGReader.cpp
    DWGImporter.cpp
    # ======================= 导出功能 BEGIN =======================
    # DWG 导出（LibreDWG 写接口）：对象 -> Entity -> .dwg
    DWGCompose.cpp
    DWGCompose.h
    DWGWriter.cpp
    DWGWriter.h
    # ======================= 导出功能 END =========================
    DWGExtract.cpp
    DWGHatch.cpp
    DWGShape.cpp
    DWGText.cpp
    DWGText.h
    DWGSurfaceMesh.cpp
    DWGSurfaceMesh.h
    DWGDimstyle.cpp
    DWGDimstyle.h
    DWGDimension.cpp
    DWGDimension.h
    DWGLineCut.h
    DWGLineGroup.cpp
    DWGLineGroup.h
    PropertyShapeBinary.cpp
    PropertyShapeBinary.h
    DWGLineType.cpp
    DWGLineType.h
    MultiInstanceTest.cpp
    MultiInstanceTest.h
    DWGShapeFile.cpp
    DWGShapeFile.h
    DWGShapeViewer.cpp
    DWGShapeViewer.h
    FontResolver.cpp
    FontResolver.h
    FTTextRenderer.cpp
    FTTextRenderer.h
    TextShxRenderer.cpp
    TextShxRenderer.h
    DWGInternal.h
    DWGTypes.h
    PathUtil.h
    PreCompiled.h
)

add_library(SWDWG SHARED ${SWDWG_SRCS})

# FreeType for TrueType embedded-linetype text (GAS_LINE "GAS" etc.).
if(FREETYPE_FOUND)
    add_definitions(-DFCUseFreeType)
    target_include_directories(SWDWG SYSTEM PUBLIC ${FREETYPE_INCLUDE_DIRS})
    target_link_libraries(SWDWG ${FREETYPE_LIBRARIES})
endif()

if(SWDWG_LIBREDWG_INCLUDE_DIR)
    target_include_directories(SWDWG PRIVATE "${SWDWG_LIBREDWG_INCLUDE_DIR}")
endif()

target_link_libraries(SWDWG ${SWDWG_LIBS})

if(SWDWG_LIBREDWG_LIB)
    target_link_libraries(SWDWG "${SWDWG_LIBREDWG_LIB}")
    if(MSVC)
        # The static libredwg.lib was built against the dynamic MSVC CRT (/MD)
        # and references the UCRT stdio import symbols. Ensure the dynamic
        # runtime and the legacy stdio shims are linked.
        target_compile_options(SWDWG PRIVATE /wd4996)
        target_link_libraries(SWDWG legacy_stdio_definitions)
    endif()
endif()


SET_BIN_DIR(SWDWG SWDWG /Mod/SWDWG)
SET_PYTHON_PREFIX_SUFFIX(SWDWG)

install(TARGETS SWDWG DESTINATION ${CMAKE_INSTALL_LIBDIR})
