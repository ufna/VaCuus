"""Recreate the material-box demo with the oldest supported Unreal Editor.

UnrealEditor-Cmd <project.uproject> -run=pythonscript \
    -script=<plugin>/Tools/author_material_box_demo.py -EnablePlugins=PythonScriptPlugin

The committed package was authored on UE 5.6. A newer editor may be used with
VACUUS_MATERIAL_BOX_PROBE=1 to validate the graph in a temporary /Game package;
that mode never saves its temporary package or the committed plugin asset.
"""

import os
import unreal

PROBE = os.environ.get("VACUUS_MATERIAL_BOX_PROBE") == "1"
DIRECTORY = "/Game" if PROBE else "/VaCuus/Demo"
NAME = "VaCuusMaterialBoxAuthoringProbe" if PROBE else "M_VaCuusDemo_RoundedBox"
PATH = DIRECTORY + "/" + NAME
CODE = """// Rounded square in UV0 (sliced by material-box), radius = the whole corner, AA in screen space.
const float Radius = 0.5;
const float Border = 0.12;
float2 q = abs(UV - 0.5) - (0.5 - Radius);
float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - Radius;
float aa = max(fwidth(d), 1e-5);
float outer = saturate(0.5 - d / aa);
float inner = saturate(0.5 - (d + Border) / aa);
float ring = outer - inner;
// Fill: a gradient along slot 4, continuous across the slice.
float3 fill = lerp(float3(0.10, 0.45, 0.85), float3(0.60, 0.20, 0.80), saturate(N.x));
float3 gold = float3(1.0, 0.82, 0.29);
float a = inner * 0.85 + ring;
float3 rgb = (fill * inner * 0.85 + gold * ring) / max(a, 1e-4);
return float4(rgb, a);
"""

lib = unreal.MaterialEditingLibrary
assets = unreal.EditorAssetLibrary
if not PROBE and not unreal.SystemLibrary.get_engine_version().startswith("5.6."):
    raise RuntimeError("Use UE 5.6 to retain package-format compatibility; use probe mode on newer engines")
if assets.does_asset_exist(PATH):
    if PROBE:
        raise RuntimeError("Probe package already exists; refusing to replace unrelated content")
    assets.delete_asset(PATH)

mat = unreal.AssetToolsHelpers.get_asset_tools().create_asset(
    NAME, DIRECTORY, unreal.Material, unreal.MaterialFactoryNew()
)
assert mat is not None, "create_asset failed"
mat.set_editor_property("material_domain", unreal.MaterialDomain.MD_UI)
mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)
uv = lib.create_material_expression(mat, unreal.MaterialExpressionTextureCoordinate, -600, -100)
uv.set_editor_property("coordinate_index", 0)
normalized = lib.create_material_expression(mat, unreal.MaterialExpressionTextureCoordinate, -600, 100)
normalized.set_editor_property("coordinate_index", 4)
shader = lib.create_material_expression(mat, unreal.MaterialExpressionCustom, -350, 0)
shader.set_editor_property("description", "RoundedBox")
shader.set_editor_property("code", CODE)
shader.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT4)
inputs = []
for name in ("UV", "N"):
    pin = unreal.CustomInput()
    pin.set_editor_property("input_name", name)
    inputs.append(pin)
shader.set_editor_property("inputs", inputs)
assert lib.connect_material_expressions(uv, "", shader, "UV")
assert lib.connect_material_expressions(normalized, "", shader, "N")
for channels, prop in [((True, True, True, False), unreal.MaterialProperty.MP_EMISSIVE_COLOR),
                       ((False, False, False, True), unreal.MaterialProperty.MP_OPACITY)]:
    mask = lib.create_material_expression(mat, unreal.MaterialExpressionComponentMask, -100, 0)
    for channel, enabled in zip("rgba", channels):
        mask.set_editor_property(channel, enabled)
    assert lib.connect_material_expressions(shader, "", mask, "")
    assert lib.connect_material_property(mask, "", prop)
lib.recompile_material(mat)
if not PROBE:
    assert assets.save_asset(PATH, only_if_is_dirty=False), "save_asset failed"
unreal.log("VaCuus material-box demo graph authored successfully")
