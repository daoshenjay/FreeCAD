# SPDX-License-Identifier: LGPL-2.1-or-later

add_subdirectory(App)
if(BUILD_GUI)
    add_subdirectory(Gui)
endif(BUILD_GUI)

set(SWDWG_Scripts
    Init.py
    # ======================= 导出功能 BEGIN =======================
    # File > Export 的 DWG 适配模块（FreeCAD 导出注册表只认 Python 模块名）
    export_dwg.py
    # ======================= 导出功能 END =========================
)

if(BUILD_GUI)
    list (APPEND SWDWG_Scripts InitGui.py)
endif(BUILD_GUI)

add_custom_target(SWDWGScripts ALL
    SOURCES ${SWDWG_Scripts}
)

fc_target_copy_resource_flat(SWDWGScripts
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_BINARY_DIR}/Mod/SWDWG
    ${SWDWG_Scripts}
)

install(
    FILES
        ${SWDWG_Scripts}
    DESTINATION
        Mod/SWDWG
)
