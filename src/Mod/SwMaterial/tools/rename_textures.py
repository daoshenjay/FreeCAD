# SPDX-License-Identifier: LGPL-2.1-or-later
"""Rename the bundled textures to Chinese file names.

Run from the repository root::

    pixi run python src/Mod/SwMaterial/tools/rename_textures.py --dry-run
    pixi run python src/Mod/SwMaterial/tools/rename_textures.py --apply
"""

import argparse
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "Resources", "textures")

# phrases that must be translated as a whole (checked first, longest first)
PHRASES = [
    ("blind road", "盲道"),
    ("blind_road", "盲道"),
    ("side walk", "人行道"),
    ("sidewalk", "人行道"),
    ("top course", "面层"),
    ("base course", "基层"),
    ("leveling binder", "找平层"),
    ("cold milling", "铣刨"),
    ("retaining wall", "挡土墙"),
    ("planting", "种植"),
    ("pavement", "铺装"),
    ("asphalt", "沥青"),
    ("shoulder", "路肩"),
    ("granite", "花岗岩"),
    ("marble", "大理石"),
    ("concrete", "混凝土"),
    ("aggregate", "骨料"),
    ("aggr", "骨料"),
    ("gravel", "砾石"),
    ("pebble", "卵石"),
    ("cobble", "毛石"),
    ("brick", "砖"),
    ("tile", "瓷砖"),
    ("block", "砌块"),
    ("paving", "铺装"),
    ("paver", "铺装"),
    ("wood", "木纹"),
    ("timber", "木材"),
    ("walnut", "胡桃木"),
    ("steel", "钢"),
    ("glass", "玻璃"),
    ("plaster", "抹灰"),
    ("stucco", "拉毛"),
    ("rust", "锈蚀"),
    ("bump", "凹凸"),
]

TOKENS = {
    "leaf": "叶片",
    "leaves": "叶片",
    "leafcard": "叶片卡",
    "bark": "树皮",
    "trunk": "树干",
    "branch": "枝干",
    "branches": "枝干",
    "twig": "枝条",
    "flower": "花朵",
    "grass": "草地",
    "palm": "棕榈",
    "cypress": "柏树",
    "conifer": "针叶",
    "oak": "橡木",
    "ash": "白蜡",
    "acacia": "刺槐",
    "ficus": "榕树",
    "quercus": "栎树",
    "rubra": "红栎",
    "picea": "云杉",
    "zelkova": "榉树",
    "salix": "柳树",
    "babylonica": "垂柳",
    "platanus": "悬铃木",
    "jacaranda": "蓝花楹",
    "hibiscus": "木槿",
    "ixora": "龙船花",
    "koelreuteria": "栾树",
    "citrus": "柑橘",
    "sinensis": "中华",
    "aurantium": "酸橙",
    "chinensis": "中华",
    "serrata": "齿叶",
    "bipinnata": "羽叶",
    "green": "绿色",
    "red": "红色",
    "white": "白色",
    "orange": "橙色",
    "half": "半",
    "bunch": "簇",
    "stalk": "茎",
    "date": "枣",
    "road": "道路",
    "wall": "墙面",
    "base": "基础",
    "course": "层",
    "layer": "层",
    "detail": "细节",
    "default": "默认",
    "billboard": "广告牌",
    "grassland": "草地",
    "water": "水面",
    "sand": "砂",
    "soil": "土壤",
    "rock": "岩石",
    "stone": "石材",
    "metal": "金属",
    "rusty": "锈蚀",
    "panel": "板材",
    "board": "板材",
    "sheet": "板材",
    "floor": "地面",
    "ceiling": "顶棚",
    "facade": "立面",
    "curtain": "幕墙",
    "roof": "屋面",
    "skp": "组件",
    "plantingrhombicskeleton": "菱形骨架护坡",
    "rhombic": "菱形",
    "skeleton": "骨架",
    "area": "区域",
    "areas": "区域",
    "short": "短",
    "long": "长",
    "light": "浅色",
    "dark": "深色",
    "back": "背面",
    "backlight": "逆光",
    "greenish": "偏绿",
    "grey": "灰色",
    "gray": "灰色",
    "black": "黑色",
    "brown": "棕色",
    "yellow": "黄色",
    "blue": "蓝色",
    "pink": "粉色",
    "old": "旧",
    "new": "新",
    "clean": "干净",
    "dirty": "脏污",
    "wet": "潮湿",
    "dry": "干燥",
    "cut": "切割",
    "broken": "破损",
    "crack": "裂纹",
    "joint": "接缝",
    "edge": "边缘",
    "corner": "转角",
    "side": "侧面",
    "walk": "步行",
    "top": "顶面",
    "bottom": "底面",
    "front": "正面",
    "inner": "内侧",
    "outer": "外侧",
    "arch": "拱形",
    "hexagon": "六边形",
    "rhombic": "菱形",
    "untitled": "未命名",
    "rusty": "锈蚀",
    "galvanized": "镀锌",
    "existing": "现有",
    "rough": "粗糙",
    "smooth": "光滑",
    "ground": "地面",
    "old": "旧",
}

# words that only describe the file, not the material
NOISE = {
    "color", "colour", "diffuse", "diffuse2", "normal", "specular", "opacity",
    "transparency", "tr", "nor", "db", "fc", "mt", "pm", "bl", "bf", "lod",
    "aa", "cd", "bd", "ce", "fa", "fe", "ae", "ca", "hl", "rt", "ed", "fde",
    "mat", "jpg", "png", "and", "of", "the", "test", "final", "copy", "v1",
    "v2", "v3", "model", "default", "layer", "detail", "areas", "date", "fall",
    "skp", "yep", "gyl", "gylllll", "refol", "qingmo", "langyu", "camei",
}


def has_chinese(text):
    return bool(re.search(r"[\u4e00-\u9fff]", text))


def translate(stem):
    text = re.sub(r"(?<=[a-z0-9])(?=[A-Z])", "_", stem)
    text = text.replace("-", "_").replace(" ", "_").lower()
    for phrase, chinese in sorted(PHRASES, key=lambda item: -len(item[0])):
        text = text.replace(phrase.replace(" ", "_"), chinese)
        text = text.replace(phrase.replace("_", ""), chinese)
    parts = [part for part in re.split(r"[^0-9a-z\u4e00-\u9fff]+", text) if part]
    out = []
    for part in parts:
        if has_chinese(part):
            out.append(part)
        elif part.isdigit():
            out.append(part)
        elif part in TOKENS:
            out.append(TOKENS[part])
        elif part in NOISE:
            continue
        else:
            continue
    cleaned = []
    for part in out:
        if part.isdigit() and len(part) >= 4:
            continue  # drop hashes like 415431222
        if part in cleaned:
            continue  # "刺槐_..._刺槐" -> keep one
        cleaned.append(part)
    result = "_".join(cleaned)
    result = re.sub(r"_+", "_", result).strip("_")
    if not has_chinese(result):
        return ""
    return result


def plan():
    renames = []
    for category in ("pattern", "bump"):
        folder = os.path.join(ROOT, category)
        if not os.path.isdir(folder):
            continue
        used = set()
        counter = 0
        for filename in sorted(os.listdir(folder)):
            path = os.path.join(folder, filename)
            if not os.path.isfile(path):
                continue
            stem, ext = os.path.splitext(filename)
            if has_chinese(stem) and not re.search(r"[A-Za-z]{2,}", stem):
                continue  # already Chinese
            counter += 1
            chinese = translate(stem)
            if not chinese:
                prefix = "凹凸" if category == "bump" else "纹理"
                chinese = "{}_{:03d}".format(prefix, counter)
            candidate = chinese + ext.lower()
            index = 2
            while candidate in used or (
                os.path.exists(os.path.join(folder, candidate)) and os.path.normcase(candidate) != os.path.normcase(filename)
            ):
                candidate = "{}_{}{}".format(chinese, index, ext.lower())
                index += 1
            used.add(candidate)
            if os.path.normcase(candidate) != os.path.normcase(filename):
                renames.append((category, filename, candidate))
    return renames


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--apply", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    renames = plan()
    for category, old, new in renames:
        print("{}/{}  ->  {}".format(category, old, new))
    print("total: {}".format(len(renames)))

    if args.apply:
        for category, old, new in renames:
            folder = os.path.join(ROOT, category)
            os.rename(os.path.join(folder, old), os.path.join(folder, new))
        print("renamed {}".format(len(renames)))
    return 0


if __name__ == "__main__":
    sys.exit(main())




