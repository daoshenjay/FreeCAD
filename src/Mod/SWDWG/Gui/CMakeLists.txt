# SPDX-License-Identifier: LGPL-2.1-or-later

include_directories(
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}
)

set(SWDWGGui_LIBS
    SWDWG
    FreeCADGui
)

qt_add_resources(SWDWG_QRC_SRCS Resources/SWDWG.qrc)

SET(SWDWGGui_SRCS
    ${SWDWG_QRC_SRCS}
    AppSWDWGGui.cpp
    Command.cpp
    PreCompiled.h
    ViewProviderDWGLineGroup.cpp
    ViewProviderDWGLineGroup.h
    ViewProviderDWGText.cpp
    ViewProviderDWGText.h
    ViewProviderDWGSurfaceMesh.cpp
    ViewProviderDWGSurfaceMesh.h
    ViewProviderDWGDimension.cpp
    ViewProviderDWGDimension.h
    ViewProviderMultiInstanceTest.cpp
    ViewProviderMultiInstanceTest.h
)

add_library(SWDWGGui SHARED ${SWDWGGui_SRCS})
target_link_libraries(SWDWGGui ${SWDWGGui_LIBS})

SET_BIN_DIR(SWDWGGui SWDWGGui /Mod/SWDWG)
SET_PYTHON_PREFIX_SUFFIX(SWDWGGui)

install(TARGETS SWDWGGui DESTINATION ${CMAKE_INSTALL_LIBDIR})

# The module icon is also installed as a plain resource so that Python code and
# documentation can reference it by path (FreeCAD.getResourceDir() +
# "Mod/SWDWG/Resources/..."), the same layout as the Material and Import modules.
install(
    DIRECTORY
        Resources
    DESTINATION
        "${CMAKE_INSTALL_DATADIR}/Mod/SWDWG"
)
