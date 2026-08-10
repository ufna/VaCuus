# M5 Task 5: authors the six /VaCuus/Spike/* material-decorator fixtures.
#
# Run (no editor may be open):
#   UnrealEditor-Cmd <host>.uproject -run=pythonscript \
#     -script=".../author_spike_materials.py" -EnablePlugins=PythonScriptPlugin
#
# WHY THIS FILE EXISTS AT ALL, and why it is dated later than the assets: the README
# beside it records that "the assets themselves were authored once by an editor python
# run", but that run was never committed -- so the graphs lived only inside the .uasset
# files. That became load-bearing during the 5.6 port: an .uasset carries the
# FileVersionUE5 of the editor that saved it (5.8 writes 1018), and a package one past
# an engine's ceiling is refused outright by the loader
# (FPackageFileSummary::IsFileVersionTooNew, PackageFileSummary.h:345-347, reached from
# LinkerLoad.cpp:1596) -- 5.6's ceiling is 1017. Since content is only forward
# compatible, the fixtures have to be AUTHORED on the oldest engine the plugin claims,
# and authoring needs the script. The graphs below were read back out of the 5.8 assets
# through unreal.MaterialEditingLibrary, node for node and link for link.
#
# The sibling m5-t6-worldspace/author_world_panel_material.py does the same job for
# /VaCuus/M_VaCuusWorldPanel, and its graph was the control that proved the read-back
# method: re-dumping the 5.8 asset reproduced that script's ten nodes exactly.
#
# What each fixture is for:
#   Opaque / Translucent / Additive -- the blend matrix the replay pass maps onto the
#       RT's single One/InvSrcAlpha state (VaCuusMaterial.usf); all MD_UI.
#   MID     -- has the two parameters vacuus.MatSpike.MID drives every game frame
#              (SpikeScalar, SpikeTex), to prove the proxy picks up per-frame changes.
#   Anim    -- Time-driven, so a frozen pixel is visible when forced republish is off.
#   WrongDomain -- deliberately MD_SURFACE and deliberately EMPTY: the refusal fixture
#              (VaCuusMaterialTest.cpp's GWrongDomainPath). Its whole content is its
#              domain, so "0 expressions" is the specification, not an omission.

import os

import unreal

# The plugin mount is the real destination. VACUUS_SPIKE_ROOT exists so the script can be
# aimed at a throwaway /Game path and its output diffed against the committed fixtures --
# which is how the graphs below were VERIFIED before 5.6 ever ran them: authoring into
# /Game/SpikeGen on 5.8 and re-dumping reproduced all six, node for node and link for link.
ROOT = os.environ.get("VACUUS_SPIKE_ROOT", "/VaCuus/Spike")
lib = unreal.MaterialEditingLibrary
asset_tools = unreal.AssetToolsHelpers.get_asset_tools()

MD_UI = unreal.MaterialDomain.MD_UI
MD_SURFACE = unreal.MaterialDomain.MD_SURFACE


def new_material(name, domain, blend_mode):
    full = ROOT + "/" + name
    if unreal.EditorAssetLibrary.does_asset_exist(full):
        unreal.EditorAssetLibrary.delete_asset(full)
    mat = asset_tools.create_asset(name, ROOT, unreal.Material, unreal.MaterialFactoryNew())
    assert mat is not None, "create_asset failed for " + full
    mat.set_editor_property("material_domain", domain)
    mat.set_editor_property("blend_mode", blend_mode)
    return mat, full


def finish(mat, full):
    lib.recompile_material(mat)
    ok = unreal.EditorAssetLibrary.save_asset(full, only_if_is_dirty=False)
    unreal.log("authored {}: {}".format(full, ok))
    assert ok, "save_asset failed for " + full


def vector_param(mat, name, rgba, x, y):
    e = lib.create_material_expression(mat, unreal.MaterialExpressionVectorParameter, x, y)
    e.set_editor_property("parameter_name", name)
    e.set_editor_property("default_value", unreal.LinearColor(*rgba))
    return e


def scalar_param(mat, name, value, x, y):
    e = lib.create_material_expression(mat, unreal.MaterialExpressionScalarParameter, x, y)
    e.set_editor_property("parameter_name", name)
    e.set_editor_property("default_value", value)
    return e


# --- M_VaCuusSpike_Opaque: flat green slab, no opacity input (BLEND_OPAQUE ignores it).
mat, full = new_material("M_VaCuusSpike_Opaque", MD_UI, unreal.BlendMode.BLEND_OPAQUE)
color = vector_param(mat, "SpikeColor", (0.02, 0.22, 0.07, 1.0), -500, 0)
lib.connect_material_property(color, "RGB", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
finish(mat, full)

# --- M_VaCuusSpike_Translucent: orange, text legible through it at alpha 0.55.
mat, full = new_material("M_VaCuusSpike_Translucent", MD_UI, unreal.BlendMode.BLEND_TRANSLUCENT)
color = vector_param(mat, "SpikeColor", (1.0, 0.35, 0.05, 1.0), -500, 0)
alpha = scalar_param(mat, "SpikeScalar", 0.55, -500, 250)
lib.connect_material_property(color, "RGB", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
lib.connect_material_property(alpha, "", unreal.MaterialProperty.MP_OPACITY)
finish(mat, full)

# --- M_VaCuusSpike_Additive: blue glow at full strength, destination text survives.
mat, full = new_material("M_VaCuusSpike_Additive", MD_UI, unreal.BlendMode.BLEND_ADDITIVE)
color = vector_param(mat, "SpikeColor", (0.05, 0.35, 0.9, 1.0), -500, 0)
alpha = scalar_param(mat, "SpikeScalar", 1.0, -500, 250)
lib.connect_material_property(color, "RGB", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
lib.connect_material_property(alpha, "", unreal.MaterialProperty.MP_OPACITY)
finish(mat, full)

# --- M_VaCuusSpike_MID: tex * tint, both drivable from a UMaterialInstanceDynamic.
mat, full = new_material("M_VaCuusSpike_MID", MD_UI, unreal.BlendMode.BLEND_TRANSLUCENT)
tex = lib.create_material_expression(mat, unreal.MaterialExpressionTextureSampleParameter2D, -700, -100)
tex.set_editor_property("parameter_name", "SpikeTex")
tex.set_editor_property("sampler_type", unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)
default_tex = unreal.EditorAssetLibrary.load_asset("/Engine/EngineResources/DefaultTexture")
if default_tex:
    tex.set_editor_property("texture", default_tex)
tint = vector_param(mat, "SpikeColor", (1.0, 1.0, 1.0, 1.0), -700, 150)
mul = lib.create_material_expression(mat, unreal.MaterialExpressionMultiply, -350, 0)
lib.connect_material_expressions(tex, "RGB", mul, "A")
lib.connect_material_expressions(tint, "RGB", mul, "B")
alpha = scalar_param(mat, "SpikeScalar", 0.7, -700, 380)
lib.connect_material_property(mul, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
lib.connect_material_property(alpha, "", unreal.MaterialProperty.MP_OPACITY)
finish(mat, full)

# --- M_VaCuusSpike_Anim: frac(u*4 + t*0.5) sweeps a two-colour stripe. Time is the
# point: between publishes the composite cannot re-evaluate a material, so with forced
# republish off this material is what visibly FREEZES (README run B).
mat, full = new_material("M_VaCuusSpike_Anim", MD_UI, unreal.BlendMode.BLEND_TRANSLUCENT)
uv = lib.create_material_expression(mat, unreal.MaterialExpressionTextureCoordinate, -1100, -100)
uv.set_editor_property("coordinate_index", 0)
uv.set_editor_property("u_tiling", 1.0)
uv.set_editor_property("v_tiling", 1.0)
mask = lib.create_material_expression(mat, unreal.MaterialExpressionComponentMask, -900, -100)
mask.set_editor_property("r", True)
mask.set_editor_property("g", False)
mask.set_editor_property("b", False)
mask.set_editor_property("a", False)
lib.connect_material_expressions(uv, "", mask, "")
stripes = lib.create_material_expression(mat, unreal.MaterialExpressionMultiply, -700, -100)
stripes.set_editor_property("const_b", 4.0)
lib.connect_material_expressions(mask, "", stripes, "A")
time = lib.create_material_expression(mat, unreal.MaterialExpressionTime, -1100, 150)
speed = lib.create_material_expression(mat, unreal.MaterialExpressionMultiply, -900, 150)
speed.set_editor_property("const_b", 0.5)
lib.connect_material_expressions(time, "", speed, "A")
phase = lib.create_material_expression(mat, unreal.MaterialExpressionAdd, -550, 0)
lib.connect_material_expressions(stripes, "", phase, "A")
lib.connect_material_expressions(speed, "", phase, "B")
frac = lib.create_material_expression(mat, unreal.MaterialExpressionFrac, -400, 0)
lib.connect_material_expressions(phase, "", frac, "")
warm = lib.create_material_expression(mat, unreal.MaterialExpressionConstant3Vector, -400, -250)
warm.set_editor_property("constant", unreal.LinearColor(1.0, 0.15, 0.05, 1.0))
cool = lib.create_material_expression(mat, unreal.MaterialExpressionConstant3Vector, -400, 250)
cool.set_editor_property("constant", unreal.LinearColor(0.05, 0.2, 1.0, 1.0))
lerp = lib.create_material_expression(mat, unreal.MaterialExpressionLinearInterpolate, -200, 0)
lib.connect_material_expressions(warm, "", lerp, "A")
lib.connect_material_expressions(cool, "", lerp, "B")
lib.connect_material_expressions(frac, "", lerp, "Alpha")
alpha = scalar_param(mat, "SpikeScalar", 0.85, -200, 380)
lib.connect_material_property(lerp, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
lib.connect_material_property(alpha, "", unreal.MaterialProperty.MP_OPACITY)
finish(mat, full)

# --- M_VaCuusSpike_WrongDomain: MD_SURFACE and empty. The refusal fixture.
mat, full = new_material("M_VaCuusSpike_WrongDomain", MD_SURFACE, unreal.BlendMode.BLEND_OPAQUE)
finish(mat, full)

unreal.log("all six spike materials authored")
