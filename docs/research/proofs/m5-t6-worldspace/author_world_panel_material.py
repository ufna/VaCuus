# M5 Task 6: authors /VaCuus/M_VaCuusWorldPanel -- the ONE world-panel preset that
# replaces UMG's six Widget3DPassThrough assets (M5 spec 2(i)).
#
# Run (no editor may be open):
#   UnrealEditor-Cmd VcHost.uproject -run=pythonscript \
#     -script=".../author_world_panel_material.py" -EnablePlugins=PythonScriptPlugin
#
# Why an editor python run and not runtime construction: runtime-constructed
# UMaterials cannot compile shaders outside the editor (the Task 5 spike's recorded
# finding (2), spec 3.3 outcome note) -- the asset is authored once here and
# committed; the component references it by path, so cooking follows the reference.
#
# The graph (all decisions cited in VaCuusWorldComponent.cpp / the M5 spec):
#   sample = TextureSampleParameter2D "VaCuusUI"  (linear-color: the RT is created
#            non-sRGB; its pixels are display-encoded by the replay contract)
#   decoded = lerp(sample.rgb, pow(sample.rgb, 2.2), "VaCuusDecodeSRGB")
#            -- the WS-GAMMA A/B knob, runtime-flippable on the MID; the shipped
#            DEFAULT is the experiment's decision (see the README beside this file)
#   face   = max(saturate(TwoSidedSign), "VaCuusBackfaceOpacity")
#            -- per-component one/two-sidedness through a SCALAR, not a static
#            switch: MIDs cannot set static switches at runtime (no such setter on
#            UMaterialInstanceDynamic; static permutations compile editor-side
#            only), so the back face is killed by zeroing BOTH rgb and a -- under
#            BLEND_AlphaComposite a (0,0,0,0) source leaves dst untouched
#   Emissive = decoded * face,  Opacity = sample.a * face
#            -- rgb AND a both multiplied: the RT is premultiplied, and
#            AlphaComposite adds One*srcRGB, so opacity-only masking would leave
#            the back face's color behind.

import unreal

ASSET_PATH = "/VaCuus"
ASSET_NAME = "M_VaCuusWorldPanel"
FULL_PATH = ASSET_PATH + "/" + ASSET_NAME

# The experiment's decision lands here (0 = raw, 1 = sRGB->linear decode).
DECODE_SRGB_DEFAULT = 1.0

lib = unreal.MaterialEditingLibrary
asset_tools = unreal.AssetToolsHelpers.get_asset_tools()

if unreal.EditorAssetLibrary.does_asset_exist(FULL_PATH):
    unreal.EditorAssetLibrary.delete_asset(FULL_PATH)

mat = asset_tools.create_asset(ASSET_NAME, ASSET_PATH, unreal.Material, unreal.MaterialFactoryNew())
assert mat is not None, "create_asset failed"

mat.set_editor_property("blend_mode", unreal.BlendMode.BLEND_ALPHA_COMPOSITE)
mat.set_editor_property("shading_model", unreal.MaterialShadingModel.MSM_UNLIT)
mat.set_editor_property("two_sided", True)
mat.set_editor_property("enable_responsive_aa", True)

# --- nodes ---
ts = lib.create_material_expression(mat, unreal.MaterialExpressionTextureSampleParameter2D, -900, 0)
ts.set_editor_property("parameter_name", "VaCuusUI")
ts.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
white = unreal.EditorAssetLibrary.load_asset("/Engine/EngineResources/WhiteSquareTexture")
if white:
    ts.set_editor_property("texture", white)

power = lib.create_material_expression(mat, unreal.MaterialExpressionPower, -600, -100)
power.set_editor_property("const_exponent", 2.2)
lib.connect_material_expressions(ts, "RGB", power, "Base")

decode = lib.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -600, -250)
decode.set_editor_property("parameter_name", "VaCuusDecodeSRGB")
decode.set_editor_property("default_value", DECODE_SRGB_DEFAULT)

lerp = lib.create_material_expression(mat, unreal.MaterialExpressionLinearInterpolate, -400, -100)
lib.connect_material_expressions(ts, "RGB", lerp, "A")
lib.connect_material_expressions(power, "", lerp, "B")
lib.connect_material_expressions(decode, "", lerp, "Alpha")

two_sided_sign = lib.create_material_expression(mat, unreal.MaterialExpressionTwoSidedSign, -600, 250)
saturate = lib.create_material_expression(mat, unreal.MaterialExpressionSaturate, -450, 250)
lib.connect_material_expressions(two_sided_sign, "", saturate, "")

backface = lib.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, -450, 380)
backface.set_editor_property("parameter_name", "VaCuusBackfaceOpacity")
backface.set_editor_property("default_value", 1.0)

face = lib.create_material_expression(mat, unreal.MaterialExpressionMax, -300, 300)
lib.connect_material_expressions(saturate, "", face, "A")
lib.connect_material_expressions(backface, "", face, "B")

emissive = lib.create_material_expression(mat, unreal.MaterialExpressionMultiply, -150, -50)
lib.connect_material_expressions(lerp, "", emissive, "A")
lib.connect_material_expressions(face, "", emissive, "B")

opacity = lib.create_material_expression(mat, unreal.MaterialExpressionMultiply, -150, 150)
lib.connect_material_expressions(ts, "A", opacity, "A")
lib.connect_material_expressions(face, "", opacity, "B")

lib.connect_material_property(emissive, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
lib.connect_material_property(opacity, "", unreal.MaterialProperty.MP_OPACITY)

lib.recompile_material(mat)
ok = unreal.EditorAssetLibrary.save_asset(FULL_PATH, only_if_is_dirty=False)
unreal.log("M_VaCuusWorldPanel authored and saved: {}".format(ok))
assert ok, "save_asset failed"
