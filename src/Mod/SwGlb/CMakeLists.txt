# SPDX-License-Identifier: LGPL-2.1-or-later

set(SwGlb_Scripts
    Init.py
    InitGui.py
)

set(SwGlb_Python
    swglb/__init__.py
    swglb/gltf.py
    swglb/export.py
    swglb/commands.py
    swglb/reader.py
    swglb/importer.py
    swglb/objects.py
)

set(SwGlb_Icons
    Resources/icons/SwGlbWorkbench.svg
    Resources/icons/SwGlbExport.svg
    Resources/icons/SwGlbImport.svg
)

set(SwGlb_Files
    ${SwGlb_Scripts}
    ${SwGlb_Python}
)

ADD_CUSTOM_TARGET(SwGlb ALL SOURCES ${SwGlb_Files} ${SwGlb_Icons})

fc_copy_sources(SwGlb "${CMAKE_BINARY_DIR}/Mod/SwGlb" ${SwGlb_Files})
fc_copy_sources(
    SwGlb
    "${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_DATADIR}/Mod/SwGlb"
    ${SwGlb_Icons}
)

INSTALL(
    FILES
        ${SwGlb_Scripts}
    DESTINATION
        Mod/SwGlb
)

INSTALL(
    DIRECTORY
        swglb
    DESTINATION
        Mod/SwGlb
    PATTERN "__pycache__" EXCLUDE
    PATTERN "*.pyc" EXCLUDE
)

INSTALL(
    DIRECTORY
        Resources
    DESTINATION
        "${CMAKE_INSTALL_DATADIR}/Mod/SwGlb"
)




