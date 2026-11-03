# SPDX-License-Identifier: LGPL-2.1-or-later

set(SwMaterial_Scripts
    Init.py
    InitGui.py
)

set(SwMaterial_Python
    swmaterial/__init__.py
    swmaterial/core.py
    swmaterial/paths.py
    swmaterial/mapping.py
    swmaterial/panel.py
    swmaterial/texture.py
    swmaterial/textures.py
    swmaterial/commands.py
)

set(SwMaterial_Icons
    Resources/icons/SwMaterialWorkbench.svg
    Resources/icons/SwMaterialPanel.svg
    Resources/icons/SwMaterialApply.svg
    Resources/icons/SwMaterialFaces.svg
    Resources/icons/SwMaterialClear.svg
    Resources/icons/SwMaterialTexture.svg
    Resources/icons/SwMaterialTextureOff.svg
)

set(SwMaterial_Files
    ${SwMaterial_Scripts}
    ${SwMaterial_Python}
)

ADD_CUSTOM_TARGET(SwMaterial ALL SOURCES ${SwMaterial_Files} ${SwMaterial_Icons})

fc_copy_sources(SwMaterial "${CMAKE_BINARY_DIR}/Mod/SwMaterial" ${SwMaterial_Files})
fc_copy_sources(
    SwMaterial
    "${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_DATADIR}/Mod/SwMaterial"
    ${SwMaterial_Icons}
)

INSTALL(
    FILES
        ${SwMaterial_Scripts}
    DESTINATION
        Mod/SwMaterial
)

INSTALL(
    DIRECTORY
        swmaterial
    DESTINATION
        Mod/SwMaterial
    PATTERN "__pycache__" EXCLUDE
    PATTERN "*.pyc" EXCLUDE
)

# Installs icons, the built-in texture library and its previews.  The textures
# are data only, so they are not copied into the build tree (300+ images).
INSTALL(
    DIRECTORY
        Resources
    DESTINATION
        "${CMAKE_INSTALL_DATADIR}/Mod/SwMaterial"
)

