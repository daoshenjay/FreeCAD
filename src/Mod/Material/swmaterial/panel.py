# SPDX-License-Identifier: LGPL-2.1-or-later

"""The SwMaterial dock: material browser, texture library and mapping options."""

import os

from PySide import QtCore, QtGui, QtWidgets

import FreeCAD as App
import FreeCADGui as Gui

from . import core, mapping as mapping_module, texture, textures

STYLE = """
QGroupBox {
    font-weight: 600;
    border: 1px solid palette(mid);
    border-radius: 5px;
    margin-top: 9px;
    padding: 6px 6px 6px 6px;
}
QGroupBox::title {
    subcontrol-origin: margin;
    subcontrol-position: top left;
    left: 8px;
    padding: 0 4px;
}
QPushButton#swPrimary {
    font-weight: 600;
    padding: 6px;
}
QPushButton {
    padding: 3px;
}
QLineEdit#swPath {
    color: palette(dark);
    background: palette(base);
    border: 1px solid palette(mid);
    border-radius: 3px;
    padding: 2px 4px;
}
QLabel#swHint {
    color: palette(dark);
}
QLabel#swInfo {
    color: palette(dark);
}
QListWidget {
    border: 1px solid palette(mid);
    border-radius: 4px;
}
"""


def _swatch(entry, size=18):
    """A small colour square used as list icon."""
    pixmap = QtGui.QPixmap(size, size)
    color = entry.diffuse
    pixmap.fill(QtGui.QColor(int(color[0] * 255), int(color[1] * 255), int(color[2] * 255)))
    return QtGui.QIcon(pixmap)


def _placeholder_icon(size=72):
    pixmap = QtGui.QPixmap(size, size)
    pixmap.fill(QtGui.QColor(232, 232, 232))
    painter = QtGui.QPainter(pixmap)
    painter.setPen(QtGui.QColor(150, 150, 150))
    painter.drawRect(0, 0, size - 1, size - 1)
    painter.drawText(pixmap.rect(), QtCore.Qt.AlignCenter, "?")
    painter.end()
    return QtGui.QIcon(pixmap)


class MaterialPanel(QtWidgets.QDockWidget):
    """Material browser, texture library and the assignment actions."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setObjectName("SwMaterialPanel")
        self.setWindowTitle("SwMaterial / 材质")
        self.setMinimumWidth(300)
        self.setStyleSheet(STYLE)

        self._materials = core.load_materials()
        self._placeholder = _placeholder_icon()
        self._suspend_live = True
        self._active = None
        self._highlight_obj = None
        self._live_timer = QtCore.QTimer(self)
        self._live_timer.setSingleShot(True)
        self._live_timer.setInterval(140)
        self._live_timer.timeout.connect(self._safe_live_apply)

        self._selection_signature = None
        self._sync_timer = QtCore.QTimer(self)
        self._sync_timer.setInterval(350)
        self._sync_timer.timeout.connect(self._safe_sync_selection)

        body = QtWidgets.QWidget(self)
        layout = QtWidgets.QVBoxLayout(body)
        layout.setContentsMargins(6, 6, 6, 6)
        layout.setSpacing(6)

        self.tabs = QtWidgets.QTabWidget(body)
        self.tabs.addTab(self._build_material_tab(), "材质 / Material")
        self.tabs.addTab(self._build_texture_tab(), "贴图 / Texture")
        layout.addWidget(self.tabs, 1)

        self.status = QtWidgets.QLabel(body)
        self.status.setObjectName("swInfo")
        self.status.setWordWrap(True)
        layout.addWidget(self.status)

        self.setWidget(body)

        self._fill_materials()
        self._fill_textures()
        self._suspend_live = False
        self._sync_timer.start()
        self.refresh_status()

    # -- small widgets ---------------------------------------------------
    def _spin(self, parent, value, minimum=0.0, maximum=100.0, decimals=2, width=54, suffix=""):
        spin = QtWidgets.QDoubleSpinBox(parent)
        spin.setRange(minimum, maximum)
        spin.setDecimals(decimals)
        spin.setValue(value)
        spin.setButtonSymbols(QtWidgets.QAbstractSpinBox.NoButtons)
        spin.setAlignment(QtCore.Qt.AlignRight)
        spin.setMaximumWidth(width)
        if suffix:
            spin.setSuffix(suffix)
        return spin

    def _row(self, parent, label, widgets, label_width=58):
        row = QtWidgets.QWidget(parent)
        layout = QtWidgets.QHBoxLayout(row)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(3)
        text = QtWidgets.QLabel(label, row)
        text.setMinimumWidth(label_width)
        layout.addWidget(text)
        for widget in widgets:
            layout.addWidget(widget, 1)
        return row

    # -- material tab ----------------------------------------------------
    def _build_material_tab(self):
        page = QtWidgets.QWidget(self)
        layout = QtWidgets.QVBoxLayout(page)
        layout.setContentsMargins(0, 6, 0, 0)
        layout.setSpacing(6)

        self.filter = QtWidgets.QLineEdit(page)
        self.filter.setPlaceholderText("搜索材质 / filter materials")
        self.filter.setClearButtonEnabled(True)
        self.filter.textChanged.connect(self._fill_materials)
        layout.addWidget(self.filter)

        self.list = QtWidgets.QListWidget(page)
        self.list.setIconSize(QtCore.QSize(18, 18))
        self.list.setAlternatingRowColors(True)
        self.list.itemDoubleClicked.connect(lambda _item: self.apply_to_objects())
        layout.addWidget(self.list, 1)

        options = QtWidgets.QGroupBox("应用到 / apply as", page)
        options_layout = QtWidgets.QFormLayout(options)
        options_layout.setSpacing(4)
        self.override = QtWidgets.QCheckBox("覆盖材质自带的参数", options)
        self.override.setToolTip("勾选后才使用下面的高光/透明度，否则沿用材质库里的值")
        self.shininess = self._spin(options, 0.5, 0.0, 1.0, decimals=2, width=70)
        self.shininess.setSingleStep(0.05)
        self.shininess.setEnabled(False)
        self.transparency = QtWidgets.QSpinBox(options)
        self.transparency.setRange(0, 99)
        self.transparency.setSuffix(" %")
        self.transparency.setValue(0)
        self.transparency.setEnabled(False)
        self.transparency.setButtonSymbols(QtWidgets.QAbstractSpinBox.NoButtons)
        self.transparency.setMaximumWidth(70)
        options_layout.addRow(self.override)
        options_layout.addRow("Shininess", self.shininess)
        options_layout.addRow("Transparency", self.transparency)
        layout.addWidget(options)
        self.override.toggled.connect(self.shininess.setEnabled)
        self.override.toggled.connect(self.transparency.setEnabled)

        buttons = QtWidgets.QGridLayout()
        buttons.setSpacing(4)
        self.button_apply = QtWidgets.QPushButton("应用到对象", page)
        self.button_apply.setObjectName("swPrimary")
        self.button_faces = QtWidgets.QPushButton("应用到选中面", page)
        self.button_palette = QtWidgets.QPushButton("逐面铺色", page)
        self.button_clear = QtWidgets.QPushButton("清除材质", page)
        buttons.addWidget(self.button_apply, 0, 0, 1, 2)
        buttons.addWidget(self.button_faces, 1, 0, 1, 2)
        buttons.addWidget(self.button_palette, 2, 0)
        buttons.addWidget(self.button_clear, 2, 1)
        layout.addLayout(buttons)

        self.button_apply.clicked.connect(self.apply_to_objects)
        self.button_faces.clicked.connect(self.apply_to_selected_faces)
        self.button_palette.clicked.connect(self.apply_palette)
        self.button_clear.clicked.connect(self.clear)
        return page

    # -- texture tab -----------------------------------------------------
    def _build_texture_tab(self):
        page = QtWidgets.QWidget(self)
        layout = QtWidgets.QVBoxLayout(page)
        layout.setContentsMargins(0, 6, 0, 0)
        layout.setSpacing(6)

        self.texture_category = QtWidgets.QComboBox(page)
        self.texture_category.addItem("全部 / all", None)
        for name in textures.categories():
            self.texture_category.addItem(textures.category_label(name), name)
        self.texture_category.currentIndexChanged.connect(self._fill_textures)
        layout.addWidget(self.texture_category)

        self.texture_search = QtWidgets.QLineEdit(page)
        self.texture_search.setPlaceholderText("搜索贴图 / filter textures")
        self.texture_search.setClearButtonEnabled(True)
        self.texture_search.textChanged.connect(self._fill_textures)
        layout.addWidget(self.texture_search)

        self.texture_list = QtWidgets.QListWidget(page)
        self.texture_list.setViewMode(QtWidgets.QListView.IconMode)
        self.texture_list.setIconSize(QtCore.QSize(70, 70))
        self.texture_list.setGridSize(QtCore.QSize(82, 94))
        self.texture_list.setResizeMode(QtWidgets.QListView.Adjust)
        self.texture_list.setMovement(QtWidgets.QListView.Static)
        self.texture_list.setWordWrap(True)
        self.texture_list.setUniformItemSizes(True)
        self.texture_list.setSpacing(2)
        self.texture_list.setMinimumHeight(150)
        self.texture_list.itemSelectionChanged.connect(self._texture_selected)
        self.texture_list.itemDoubleClicked.connect(lambda _item: self.apply_texture())
        layout.addWidget(self.texture_list, 1)

        self.texture_count = QtWidgets.QLabel(page)
        self.texture_count.setObjectName("swInfo")
        layout.addWidget(self.texture_count)

        self.texture_path = QtWidgets.QLineEdit(page)
        self.texture_path.setObjectName("swPath")
        self.texture_path.setReadOnly(True)
        self.texture_path.setPlaceholderText("未选择图片 / no image selected")
        layout.addWidget(self.texture_path)

        params = QtWidgets.QWidget(page)
        params_layout = QtWidgets.QVBoxLayout(params)
        params_layout.setContentsMargins(0, 0, 2, 0)
        params_layout.setSpacing(6)
        params_layout.addWidget(self._build_mapping_group(params))
        params_layout.addWidget(self._build_adjust_group(params))

        scroll = QtWidgets.QScrollArea(page)
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QtWidgets.QFrame.NoFrame)
        scroll.setMaximumHeight(360)
        scroll.setHorizontalScrollBarPolicy(QtCore.Qt.ScrollBarAlwaysOff)
        scroll.setWidget(params)
        layout.addWidget(scroll)

        self.scope = QtWidgets.QComboBox(page)
        self.scope.addItem("选中面（保留其他面）", "keep")
        self.scope.addItem("选中面（清除其他面）", "replace")
        self.scope.addItem("整个对象", "object")
        self.scope.setToolTip(
            "选了面时：保留 = 其他面继续用原来的贴图；清除 = 先删掉本对象所有贴图再贴"
        )
        layout.addWidget(self._row(page, "范围", [self.scope], label_width=56))

        self.live_update = QtWidgets.QCheckBox("参数改动立即生效", page)
        self.live_update.setChecked(True)
        self.live_update.setToolTip("勾选后，改参数会直接更新已贴图的选中对象，不用再点“贴到选中对象”")
        layout.addWidget(self.live_update)

        self.button_texture = QtWidgets.QPushButton("贴到选中对象", page)
        self.button_texture.setObjectName("swPrimary")
        self.button_texture_off = QtWidgets.QPushButton("移除贴图", page)
        self.texture_browse = QtWidgets.QPushButton("浏览本地图片…", page)

        layout.addWidget(self.button_texture)
        row = QtWidgets.QHBoxLayout()
        row.setSpacing(4)
        row.addWidget(self.button_texture_off)
        row.addWidget(self.texture_browse)
        layout.addLayout(row)

        hint = QtWidgets.QLabel("贴图只影响 3D 视图显示，不写入 STEP/IGES", page)
        hint.setObjectName("swHint")
        hint.setWordWrap(True)
        layout.addWidget(hint)

        self.button_texture.clicked.connect(self.apply_texture)
        self.button_texture_off.clicked.connect(self.remove_texture)
        self.texture_browse.clicked.connect(self.browse_texture)
        self._connect_live_updates()
        return page

    def _connect_live_updates(self):
        """Every parameter widget schedules a live re-apply of the overlay."""
        widgets = (
            self.mapping_mode,
            self.mapping_auto_center,
            self.mapping_rotation,
            self.scale_u,
            self.scale_v,
            self.scale_link,
            self.offset_u,
            self.offset_v,
            self.flip_u,
            self.flip_v,
            self.swap_uv,
            self.wrap_mode,
            self.real_size,
            self.tile_size,
        )
        for widget in widgets:
            if isinstance(widget, QtWidgets.QComboBox):
                widget.currentIndexChanged.connect(self._schedule_live)
            elif isinstance(widget, QtWidgets.QCheckBox):
                widget.toggled.connect(self._schedule_live)
            else:
                widget.valueChanged.connect(self._schedule_live)
        for spin in list(self.mapping_direction) + list(self.mapping_origin):
            spin.valueChanged.connect(self._schedule_live)

    def _schedule_live(self, *_args):
        if self._suspend_live or not self.live_update.isChecked():
            return
        self._live_timer.start()

    def _active_layer(self):
        """(object, layer index) the panel is currently editing."""
        active = getattr(self, "_active", None)
        if not active:
            return None, None
        document, name, index = active
        try:
            doc = App.getDocument(document)
        except Exception:
            return None, None
        obj = doc.getObject(name) if doc is not None else None
        if obj is None or not texture.has_texture(obj):
            return None, None
        if not (0 <= index < len(texture.read_layers(obj))):
            return None, None
        return obj, index

    def _safe_live_apply(self):
        """Timer slot: a deleted document must not raise inside Qt."""
        try:
            self._live_apply()
        except ReferenceError:
            self._active = None

    def _safe_sync_selection(self):
        try:
            self._sync_selection()
        except ReferenceError:
            self._highlight_obj = None
            self._active = None

    def _live_apply(self):
        """Re-apply the current mapping to whatever the panel is showing.

        Editing while a *face* is selected creates (or updates) an override for
        those faces, so the whole-object texture keeps its own parameters.
        """
        mapping = self.current_mapping()
        selection = core.filter_targets(Gui.Selection.getSelection())
        try:
            if not selection:
                obj, index = self._active_layer()
                if obj is None:
                    self.status.setText("没有可更新的对象：先选中对象并点一次“贴到选中对象”")
                    return
                if texture.update_layer(obj, index, mapping):
                    Gui.updateGui()
                    self.status.setText(
                        "已实时更新 {}（{}）".format(obj.Label, mapping_module.label(mapping.mode))
                    )
                return

            changed = 0
            created = 0
            for obj in selection:
                faces = self.selected_faces(obj)
                if faces:
                    index, layer = self._layer_for(obj, faces)
                    if layer is None:
                        continue
                    if layer["faces"] is None:
                        # first edit of a face that only has the base texture:
                        # give it its own layer instead of touching the base
                        texture.apply_texture(obj, layer["image"], mapping, faces=faces)
                        self._active = (
                            obj.Document.Name,
                            obj.Name,
                            len(texture.read_layers(obj)) - 1,
                        )
                        created += 1
                    else:
                        texture.update_layer(obj, index, mapping)
                        self._active = (obj.Document.Name, obj.Name, index)
                    changed += 1
                else:
                    index, layer = self._layer_for(obj, ())
                    if layer is None:
                        continue
                    texture.update_layer(obj, index, mapping)
                    self._active = (obj.Document.Name, obj.Name, index)
                    changed += 1

            if changed:
                Gui.updateGui()
                suffix = "，新建 {} 个面覆盖".format(created) if created else ""
                self.status.setText(
                    "已实时更新 {} 个对象{}{}".format(
                        changed, suffix, "（{}）".format(mapping_module.label(mapping.mode))
                    )
                )
        except Exception as exc:
            self.status.setText("更新失败：{}".format(exc))

    def _build_mapping_group(self, parent):
        box = QtWidgets.QGroupBox("映射 / mapping", parent)
        layout = QtWidgets.QVBoxLayout(box)
        layout.setContentsMargins(6, 6, 6, 4)
        layout.setSpacing(4)

        self.mapping_mode = QtWidgets.QComboBox(box)
        for identifier, text in mapping_module.MODES:
            self.mapping_mode.addItem(text, identifier)
        layout.addWidget(self.mapping_mode)

        self.mapping_direction = [
            self._spin(box, value, -100000.0, 100000.0, width=46)
            for value in (0.0, 0.0, 1.0)
        ]
        direction_row = self._row(box, "方向/轴", self.mapping_direction, label_width=56)
        direction_row.setToolTip("平面/投影的法向；柱面/球面的轴向")
        layout.addWidget(direction_row)

        self.mapping_auto_center = QtWidgets.QCheckBox("原点 = 包围盒中心", box)
        self.mapping_auto_center.setChecked(True)
        layout.addWidget(self.mapping_auto_center)

        self.mapping_origin = [
            self._spin(box, 0.0, -100000.0, 100000.0, width=46) for _ in range(3)
        ]
        origin_row = self._row(box, "原点", self.mapping_origin, label_width=56)
        origin_row.setToolTip("映射原点；球心、柱轴上的点也用它")
        layout.addWidget(origin_row)
        for spin in self.mapping_origin:
            spin.setEnabled(False)
        self.mapping_auto_center.toggled.connect(
            lambda checked: [spin.setEnabled(not checked) for spin in self.mapping_origin]
        )

        rotation = QtWidgets.QWidget(box)
        rotation_layout = QtWidgets.QHBoxLayout(rotation)
        rotation_layout.setContentsMargins(0, 0, 0, 0)
        rotation_layout.setSpacing(3)
        label = QtWidgets.QLabel("旋转", rotation)
        label.setMinimumWidth(56)
        rotation_layout.addWidget(label)
        self.mapping_rotation = self._spin(rotation, 0.0, -360.0, 360.0, decimals=1, width=56,
                                           suffix="°")
        rotation_layout.addWidget(self.mapping_rotation, 1)
        rotate_button = QtWidgets.QPushButton("+90°", rotation)
        rotate_button.setFixedWidth(42)
        rotate_button.setToolTip("每次增加 90°")
        rotate_button.clicked.connect(self._rotate_90)
        rotation_layout.addWidget(rotate_button)
        layout.addWidget(rotation)
        return box

    def _rotate_90(self):
        value = self.mapping_rotation.value() + 90.0
        while value > 180.0:
            value -= 360.0
        self.mapping_rotation.setValue(value)

    def _build_adjust_group(self, parent):
        box = QtWidgets.QGroupBox("调整 / adjustments", parent)
        layout = QtWidgets.QVBoxLayout(box)
        layout.setContentsMargins(6, 6, 6, 4)
        layout.setSpacing(4)

        self.real_size = QtWidgets.QCheckBox("实际尺寸（一张贴图 = 多少 mm）", box)
        self.real_size.setToolTip(
            "勾选后贴图按模型单位铺砖：4 m 的墙会比 1 m 的墙多铺 4 倍的砖"
        )
        layout.addWidget(self.real_size)
        self.tile_size = self._spin(box, 1000.0, 1.0, 100000.0, decimals=0, width=64)
        self.tile_size.setToolTip("一张贴图覆盖的长度（mm），例如 4×4 块 300 mm 砖 = 1200")
        self.tile_size.setEnabled(False)
        tile_row = self._row(box, "一张 =", [self.tile_size], label_width=56)
        layout.addWidget(tile_row)
        self.real_size.toggled.connect(self.tile_size.setEnabled)

        self.scale_u = self._spin(box, 1.0, 0.01, 1000.0, width=46)
        self.scale_v = self._spin(box, 1.0, 0.01, 1000.0, width=46)
        self.scale_link = QtWidgets.QCheckBox("等比", box)
        self.scale_link.setChecked(True)
        self.scale_link.setToolTip("U/V 使用同一个缩放")
        scale_row = QtWidgets.QWidget(box)
        scale_layout = QtWidgets.QHBoxLayout(scale_row)
        scale_layout.setContentsMargins(0, 0, 0, 0)
        scale_layout.setSpacing(3)
        label = QtWidgets.QLabel("缩放", scale_row)
        label.setMinimumWidth(56)
        scale_layout.addWidget(label)
        scale_layout.addWidget(self.scale_u, 1)
        scale_layout.addWidget(self.scale_v, 1)
        scale_layout.addWidget(self.scale_link)
        layout.addWidget(scale_row)
        self.scale_v.setEnabled(False)
        self.scale_link.toggled.connect(self._on_scale_link)
        self.scale_u.valueChanged.connect(self._on_scale_u)

        self.offset_u = self._spin(box, 0.0, -100.0, 100.0, width=46)
        self.offset_v = self._spin(box, 0.0, -100.0, 100.0, width=46)
        for spin, axis in ((self.offset_u, "U"), (self.offset_v, "V")):
            spin.setToolTip(
                "沿 {} 方向平移，单位是“一张贴图”：0.5 = 平移半张，1 = 正好一张".format(axis)
            )
        layout.addWidget(self._row(box, "偏移", [self.offset_u, self.offset_v], label_width=56))

        self.flip_u = QtWidgets.QCheckBox("翻转 U", box)
        self.flip_v = QtWidgets.QCheckBox("翻转 V", box)
        self.swap_uv = QtWidgets.QCheckBox("交换 U/V", box)
        flips = QtWidgets.QWidget(box)
        flip_layout = QtWidgets.QHBoxLayout(flips)
        flip_layout.setContentsMargins(0, 0, 0, 0)
        flip_layout.setSpacing(4)
        flip_layout.addWidget(self.flip_u)
        flip_layout.addWidget(self.flip_v)
        flip_layout.addWidget(self.swap_uv)
        flip_layout.addStretch(1)
        layout.addWidget(flips)

        self.wrap_mode = QtWidgets.QComboBox(box)
        for identifier, text in mapping_module.WRAPS:
            self.wrap_mode.addItem(text, identifier)
        layout.addWidget(self._row(box, "环绕", [self.wrap_mode], label_width=56))
        return box

    def _on_scale_link(self, checked):
        self.scale_v.setEnabled(not checked)
        if checked:
            self.scale_v.setValue(self.scale_u.value())

    def _on_scale_u(self, value):
        if self.scale_link.isChecked():
            self.scale_v.setValue(value)

    def current_mapping(self):
        origin = None
        if not self.mapping_auto_center.isChecked():
            origin = App.Vector(*[spin.value() for spin in self.mapping_origin])
        direction = App.Vector(*[spin.value() for spin in self.mapping_direction])
        scale_v = self.scale_u.value() if self.scale_link.isChecked() else self.scale_v.value()
        return mapping_module.Mapping(
            mode=self.mapping_mode.currentData(),
            origin=origin,
            direction=direction,
            rotation=self.mapping_rotation.value(),
            scale_u=self.scale_u.value(),
            scale_v=scale_v,
            offset_u=self.offset_u.value(),
            offset_v=self.offset_v.value(),
            flip_u=self.flip_u.isChecked(),
            flip_v=self.flip_v.isChecked(),
            swap_uv=self.swap_uv.isChecked(),
            wrap=self.wrap_mode.currentData(),
            tile=self.tile_size.value() if self.real_size.isChecked() else 0.0,
            fit=not self.real_size.isChecked(),
        )

    def _load_mapping(self, mapping):
        """Fill the widgets from a mapping (used when a textured object is picked)."""
        index = self.mapping_mode.findData(mapping.mode)
        if index >= 0:
            self.mapping_mode.setCurrentIndex(index)
        for spin, value in zip(self.mapping_direction, mapping.direction):
            spin.setValue(value)
        if mapping.origin is None:
            self.mapping_auto_center.setChecked(True)
        else:
            self.mapping_auto_center.setChecked(False)
            for spin, value in zip(self.mapping_origin, mapping.origin):
                spin.setValue(value)
        self.mapping_rotation.setValue(mapping.rotation)
        linked = abs(mapping.scale_u - mapping.scale_v) < 1e-9
        self.scale_link.setChecked(linked)
        self.scale_u.setValue(mapping.scale_u)
        self.scale_v.setValue(mapping.scale_v)
        self.offset_u.setValue(mapping.offset_u)
        self.offset_v.setValue(mapping.offset_v)
        self.flip_u.setChecked(mapping.flip_u)
        self.flip_v.setChecked(mapping.flip_v)
        self.swap_uv.setChecked(mapping.swap_uv)
        index = self.wrap_mode.findData(mapping.wrap)
        if index >= 0:
            self.wrap_mode.setCurrentIndex(index)
        self.real_size.setChecked(not mapping.fit)
        if mapping.tile > 0:
            self.tile_size.setValue(mapping.tile)
        self.tile_size.setEnabled(not mapping.fit)

    # -- follow the selection --------------------------------------------
    def _selection_signature_now(self):
        objects = core.filter_targets(Gui.Selection.getSelection())
        if not objects:
            return None
        obj = objects[0]
        return (obj.Document.Name, obj.Name, tuple(self.selected_faces(obj)))

    def _layer_for(self, obj, faces):
        """(index, layer) shown for a picked object / face.

        A picked face shows the topmost layer covering it (the override).
        A whole-object pick shows the base layer (``faces is None``) when there
        is one, otherwise the layer covering the most faces.
        """
        layers = texture.read_layers(obj)
        if not layers:
            return None, None
        total = len(obj.Shape.Faces)

        def covered(layer):
            return set(range(total)) if layer["faces"] is None else set(layer["faces"])

        if faces:
            wanted = set(faces)
            for index in range(len(layers) - 1, -1, -1):
                if wanted & covered(layers[index]):
                    return index, layers[index]
        for index, layer in enumerate(layers):
            if layer["faces"] is None:
                return index, layer
        best = max(range(len(layers)), key=lambda index: len(covered(layers[index])))
        return best, layers[best]

    def _highlight_texture(self, path):
        """Select the library entry that matches ``path`` (when it is built in)."""
        for row in range(self.texture_list.count()):
            item = self.texture_list.item(row)
            entry = item.data(QtCore.Qt.UserRole)
            if entry is not None and os.path.normcase(entry.path) == os.path.normcase(path):
                self.texture_list.setCurrentRow(row)
                return

    def _sync_selection(self):
        """Show the texture settings of the object that is selected now."""
        if self._suspend_live or self._live_timer.isActive():
            # never overwrite values the user is in the middle of editing
            return
        signature = self._selection_signature_now()
        if signature is None:
            # clicking into empty space clears the selection and its highlight
            if self._highlight_obj is not None:
                texture.clear_highlight(self._highlight_obj)
                self._highlight_obj = None
            self._selection_signature = None
            return
        if signature == self._selection_signature:
            return
        self._selection_signature = signature
        try:
            doc = App.getDocument(signature[0])
            obj = doc.getObject(signature[1]) if doc is not None else None
        except Exception:
            obj = None
        if self._highlight_obj is not None:
            texture.clear_highlight(self._highlight_obj)
            self._highlight_obj = None
        if obj is not None and texture.has_texture(obj):
            texture.set_highlight(obj, list(signature[2]) or None)
            self._highlight_obj = obj
        if obj is None or not texture.has_texture(obj):
            self._active = None
            return
        index, layer = self._layer_for(obj, signature[2])
        if layer is None:
            self._active = None
            return
        self._suspend_live = True
        try:
            self._load_mapping(layer["mapping"])
            self.texture_path.setText(layer["image"])
            self.texture_path.setToolTip(layer["image"])
            self._highlight_texture(layer["image"])
        finally:
            self._suspend_live = False
        self._active = (signature[0], signature[1], index)
        scope = "{} 个面".format(len(layer["faces"])) if layer["faces"] else "整个对象"
        self.status.setText(
            "已载入 {} 的贴图参数（{}，{}）".format(
                obj.Label, mapping_module.label(layer["mapping"].mode), scope
            )
        )

    # -- filling ---------------------------------------------------------
    def _fill_materials(self):
        text = self.filter.text().strip().lower()
        self.list.clear()
        for entry in self._materials:
            if text and text not in entry.name.lower() and text not in entry.library.lower():
                continue
            item = QtWidgets.QListWidgetItem(_swatch(entry), entry.name)
            item.setToolTip(core.describe(entry))
            item.setData(QtCore.Qt.UserRole, entry)
            self.list.addItem(item)
        if self.list.count():
            self.list.setCurrentRow(0)

    def _fill_textures(self):
        if not hasattr(self, "texture_list"):
            return
        category = self.texture_category.currentData()
        text = self.texture_search.text().strip().lower()
        items = textures.list_textures(category)
        if text:
            items = [item for item in items if text in item.name.lower()]
        self.texture_list.clear()
        for entry in items:
            icon = QtGui.QIcon(entry.thumb) if entry.thumb else self._placeholder
            item = QtWidgets.QListWidgetItem(icon, entry.name)
            item.setToolTip("{}\n{}".format(entry.name, entry.path))
            item.setData(QtCore.Qt.UserRole, entry)
            self.texture_list.addItem(item)
        self.texture_count.setText("共 {} 张贴图".format(self.texture_list.count()))

    def _texture_selected(self):
        item = self.texture_list.currentItem()
        if item is None:
            return
        entry = item.data(QtCore.Qt.UserRole)
        self.texture_path.setText(entry.path)
        self.texture_path.setToolTip(entry.path)

    # -- helpers ---------------------------------------------------------
    def current_entry(self):
        item = self.list.currentItem()
        return item.data(QtCore.Qt.UserRole) if item is not None else None

    def current_texture(self):
        """Path in the text box wins; the list is only a fallback.

        ``QListWidget.currentItem()`` keeps pointing at a row even after
        ``clearSelection()``, so trusting it made "browse local image" silently
        apply the previously highlighted library texture.
        """
        text = self.texture_path.text().strip()
        if text and os.path.isfile(text):
            return text
        item = self.texture_list.currentItem()
        if item is not None and item.isSelected():
            entry = item.data(QtCore.Qt.UserRole)
            if entry is not None:
                return entry.path
        return text

    def targets(self):
        return core.filter_targets(Gui.Selection.getSelection())

    def _overrides(self):
        if not self.override.isChecked():
            return {"transparency": None, "shininess": None}
        return {
            "transparency": self.transparency.value() / 100.0,
            "shininess": self.shininess.value(),
        }

    def refresh_status(self):
        targets = self.targets()
        if targets:
            self.status.setText("已选 {} 个对象".format(len(targets)))
        else:
            self.status.setText("请先选择可赋材质对象（如 Part/PartDesign 实体）")

    def show_texture_tab(self):
        self.tabs.setCurrentIndex(1)
        self.show()
        self.raise_()

    # -- material actions ------------------------------------------------
    def apply_to_objects(self):
        entry = self.current_entry()
        targets = self.targets()
        if entry is None or not targets:
            self.refresh_status()
            return
        done = 0
        for obj in targets:
            if core.apply_material(obj, entry, **self._overrides()):
                done += 1
        Gui.updateGui()
        self.status.setText("已给 {} 个对象赋材质：{}".format(done, entry.name))

    def selected_faces(self, obj):
        """Zero-based face indices selected for ``obj`` (empty = whole object)."""
        result = []
        for info in Gui.Selection.getSelectionEx():
            if info.Object is not obj:
                continue
            for name in info.SubElementNames:
                if name.startswith("Face"):
                    try:
                        result.append(int(name[4:]) - 1)
                    except ValueError:
                        continue
        return result

    def apply_to_selected_faces(self):
        entry = self.current_entry()
        targets = self.targets()
        if entry is None or not targets:
            self.refresh_status()
            return
        done = 0
        for obj in targets:
            faces = self.selected_faces(obj)
            if not faces:
                if core.apply_palette(obj, entry):
                    done += 1
                continue
            if core.apply_material(obj, entry, faces=faces, **self._overrides()):
                done += 1
        Gui.updateGui()
        self.status.setText("已处理 {} 个对象：{}".format(done, entry.name))

    def apply_palette(self):
        entry = self.current_entry()
        targets = self.targets()
        if entry is None or not targets:
            self.refresh_status()
            return
        done = sum(1 for obj in targets if core.apply_palette(obj, entry))
        Gui.updateGui()
        self.status.setText("已逐面铺色 {} 个对象：{}".format(done, entry.name))

    def clear(self):
        targets = self.targets()
        done = sum(1 for obj in targets if core.clear_material(obj))
        Gui.updateGui()
        self.status.setText("已恢复默认材质：{} 个对象".format(done))

    # -- texture actions -------------------------------------------------
    def browse_texture(self):
        path, _filter = QtWidgets.QFileDialog.getOpenFileName(
            self,
            "选择贴图 / choose texture",
            self.texture_path.text() or "",
            "Images (*.png *.jpg *.jpeg *.bmp *.tif *.tiff *.webp);;All files (*)",
        )
        if not path:
            return
        self.texture_list.clearSelection()
        self.texture_list.setCurrentItem(None)
        self.texture_path.setText(path)
        self.texture_path.setToolTip(path)

    def apply_texture(self):
        # glTF meshes cannot take a material (no ShapeAppearance), but they can
        # take a projected texture, so use the texture-target filter here
        targets = texture.filter_texture_targets(Gui.Selection.getSelection())
        path = self.current_texture()
        if not targets:
            self.status.setText("请先选择对象")
            return
        if not path or not os.path.isfile(path):
            self.tabs.setCurrentIndex(1)
            self.status.setText("请先选一张贴图，或点“浏览本地图片…”")
            return

        mapping = self.current_mapping()
        scope = self.scope.currentData()
        done = 0
        applied = []
        face_total = 0
        for obj in targets:
            faces = self.selected_faces(obj) or None
            if scope == "object":
                faces = None
            replace = scope == "replace"
            try:
                if texture.apply_texture(obj, path, mapping, faces=faces, replace=replace):
                    done += 1
                    face_total += len(faces) if faces else core.face_count(obj)
                    applied.append((obj.Document.Name, obj.Name, faces))
            except Exception as exc:  # report and continue with the other objects
                self.status.setText("贴图失败：{}".format(exc))
        if applied:
            first_obj = None
            try:
                first_obj = App.getDocument(applied[0][0]).getObject(applied[0][1])
            except Exception:
                first_obj = None
            if first_obj is not None:
                texture.set_highlight(first_obj, list(applied[0][2]) if applied[0][2] else None)
                self._highlight_obj = first_obj
            last_doc, last_name, _faces = applied[-1]
            try:
                last_obj = App.getDocument(last_doc).getObject(last_name)
            except Exception:
                last_obj = None
            if last_obj is not None:
                self._active = (last_doc, last_name, len(texture.read_layers(last_obj)) - 1)
            self._selection_signature = self._selection_signature_now()
        Gui.updateGui()
        scope = "{} 个面".format(face_total) if face_total and done else ""
        self.status.setText(
            "已贴图 {} 个对象{}：{}（{}）".format(
                done, "，" + scope if scope else "", os.path.basename(path),
                mapping_module.label(mapping.mode),
            )
        )

    def remove_texture(self):
        # glTF meshes carry textures too (no ShapeAppearance, no faces)
        targets = texture.filter_texture_targets(Gui.Selection.getSelection())
        done = 0
        had_faces = False
        for obj in targets:
            faces = self.selected_faces(obj) or None
            had_faces = had_faces or bool(faces)
            if texture.remove_texture(obj, faces=faces):
                done += 1
        obj, _index = self._active_layer()
        if obj is None or not texture.has_texture(obj):
            self._active = None
        self._selection_signature = self._selection_signature_now()
        Gui.updateGui()
        self.status.setText(
            "已移除贴图：{} 个对象{}".format(done, "（选中面）" if had_faces else "")
        )


_PANEL = None


def get_panel():
    """Create (once) and return the dock widget."""
    global _PANEL
    if _PANEL is None:
        _PANEL = MaterialPanel(Gui.getMainWindow())
        Gui.getMainWindow().addDockWidget(QtCore.Qt.RightDockWidgetArea, _PANEL)
    return _PANEL


def show_panel():
    panel = get_panel()
    panel.show()
    panel.raise_()
    panel.refresh_status()
    return panel


def show_texture_tab():
    panel = show_panel()
    panel.show_texture_tab()
    return panel


def toggle_panel():
    panel = get_panel()
    panel.setVisible(not panel.isVisible())
    if panel.isVisible():
        panel.refresh_status()
    return panel












