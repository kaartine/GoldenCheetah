#!/usr/bin/env python3
"""Open the committed Workout Game GLBs in a small Blender review gallery."""

from __future__ import annotations

import argparse
from array import array
from dataclasses import dataclass
import json
import math
from pathlib import Path
import sys
import traceback

import bpy


SCRIPT_DIRECTORY = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIRECTORY))
sys.path.insert(0, str(SCRIPT_DIRECTORY.parent))

from gallery_catalog import (  # noqa: E402
    GalleryAsset,
    gallery_asset_index,
    load_candidate_gallery_assets,
    load_gallery_assets,
    validate_gallery_asset_file,
)
from editor.asset_document import AssetDocument, AssetDocumentError  # noqa: E402
from render_rider_bike_audit import assemble_neutral_pose  # noqa: E402


@dataclass
class ImportedAsset:
    catalog: GalleryAsset
    object_names: tuple[str, ...]
    mesh_names: tuple[str, ...]
    material_names: tuple[tuple[str, str], ...]


IMPORTED_ASSETS: list[ImportedAsset] = []
ASSET_DOCUMENTS: dict[str, AssetDocument] = {}
REPOSITORY: Path | None = None
EDIT_ENABLED = False
SYNCING_CONTROLS = False
GPU_EVIDENCE_PATH: Path | None = None
UI_SCREENSHOT_PATH: Path | None = None


def _arguments() -> argparse.Namespace:
    arguments = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    parser = argparse.ArgumentParser(description="Open the Workout Game asset gallery")
    parser.add_argument(
        "--root",
        default=str(SCRIPT_DIRECTORY.parents[2]),
        help="GoldenCheetah repository root",
    )
    parser.add_argument(
        "--asset",
        default="",
        help="Initial asset id, display name or GLB stem",
    )
    parser.add_argument(
        "--candidate-directory",
        default="",
        help="External verified TripoSR candidate directory (review only)",
    )
    parser.add_argument("--smoke-test", action="store_true")
    parser.add_argument("--ui-smoke-test", action="store_true")
    parser.add_argument(
        "--gpu-evidence",
        default="",
        help="Write the active Blender GPU backend, vendor and renderer as JSON",
    )
    parser.add_argument(
        "--ui-screenshot",
        default="",
        help="Capture the gallery window during --ui-smoke-test",
    )
    parser.add_argument(
        "--evidence-directory",
        default="",
        help="Write gpu.json and gallery.png review evidence into this directory",
    )
    parser.add_argument(
        "--editor-smoke-test",
        action="store_true",
        help="Modify one manifest through Save; use only in a disposable workspace",
    )
    parser.add_argument(
        "--edit",
        action="store_true",
        help="Enable validated manifest material and physics editing",
    )
    return parser.parse_args(arguments)


def _srgb_channel_to_linear(value: float) -> float:
    return value / 12.92 if value <= 0.04045 else ((value + 0.055) / 1.055) ** 2.4


def _linear_channel_to_srgb(value: float) -> float:
    value = min(1.0, max(0.0, value))
    return value * 12.92 if value <= 0.0031308 else 1.055 * value ** (1.0 / 2.4) - 0.055


def _hex_to_linear_color(value: str) -> tuple[float, float, float]:
    channels = tuple(int(value[index:index + 2], 16) / 255.0 for index in (1, 3, 5))
    return tuple(_srgb_channel_to_linear(channel) for channel in channels)


def _linear_color_to_hex(value) -> str:
    channels = [
        min(255, max(0, round(_linear_channel_to_srgb(float(channel)) * 255.0)))
        for channel in value[:3]
    ]
    return "#" + "".join(f"{channel:02x}" for channel in channels)


def _gpu_details() -> dict[str, str]:
    import gpu

    platform = gpu.platform
    backend_get = getattr(platform, "backend_type_get", None)
    return {
        "backend": str(backend_get()) if backend_get is not None else "unknown",
        "vendor": str(platform.vendor_get()),
        "renderer": str(platform.renderer_get()),
        "version": str(platform.version_get()),
    }


def _record_gpu_evidence(path: Path) -> dict[str, str]:
    details = _gpu_details()
    path = path.expanduser().absolute()
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(details, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return details


def _validate_ui_screenshot(path: Path) -> None:
    image = bpy.data.images.load(str(path), check_existing=False)
    try:
        image.scale(64, 64)
        values = array("f", [0.0]) * len(image.pixels)
        image.pixels.foreach_get(values)
        samples = values[0::4]
        if not samples or max(samples) < 0.05 or max(samples) - min(samples) < 0.02:
            raise RuntimeError("gallery UI screenshot is blank")
    finally:
        bpy.data.images.remove(image)


def _active_document(scene: bpy.types.Scene) -> AssetDocument | None:
    return ASSET_DOCUMENTS.get(scene.workout_game_gallery_asset)


def _active_imported(scene: bpy.types.Scene) -> ImportedAsset:
    return IMPORTED_ASSETS[_visible_index(scene)]


def _material(scene: bpy.types.Scene, name: str) -> bpy.types.Material:
    imported = _active_imported(scene)
    mapped = dict(imported.material_names).get(name)
    material = bpy.data.materials.get(mapped) if mapped is not None else None
    if material is None:
        raise RuntimeError(
            f"imported GLB material is unavailable: {imported.catalog.asset_id} / {name}"
        )
    return material


def _principled(material: bpy.types.Material):
    if not material.use_nodes or material.node_tree is None:
        return None
    return next(
        (node for node in material.node_tree.nodes if node.type == "BSDF_PRINCIPLED"),
        None,
    )


def _set_material_preview(
    scene: bpy.types.Scene,
    name: str,
    color: tuple[float, float, float],
    roughness: float,
    metallic: float,
) -> None:
    material = _material(scene, name)
    material.diffuse_color = (*color, 1.0)
    material.roughness = roughness
    material.metallic = metallic
    node = _principled(material)
    if node is not None:
        node.inputs["Base Color"].default_value = (*color, 1.0)
        node.inputs["Roughness"].default_value = roughness
        node.inputs["Metallic"].default_value = metallic


def _material_values(scene: bpy.types.Scene, document: AssetDocument, name: str):
    override = document.material_overrides.get(name)
    material = _material(scene, name)
    if override is not None:
        return (
            _hex_to_linear_color(override["baseColorSrgb"]),
            float(override["roughness"]),
            float(override["metallic"]),
        )
    node = _principled(material)
    color = material.diffuse_color[:3]
    roughness = material.roughness
    metallic = material.metallic
    if node is not None:
        color = node.inputs["Base Color"].default_value[:3]
        roughness = node.inputs["Roughness"].default_value
        metallic = node.inputs["Metallic"].default_value
    return tuple(color), float(roughness), float(metallic)


def _material_items(_owner, context):
    document = _active_document(context.scene) if context is not None else None
    if document is None:
        return (("", "No editable material", ""),)
    return tuple((name, name, "GLB material") for name in document.material_names)


def _collision_node_items(_owner, context):
    document = _active_document(context.scene) if context is not None else None
    items = [("NONE", "No collision proxy", "External runtime physics")]
    if document is not None:
        items.extend((name, name, "GLB node reference") for name in document.node_names)
    return tuple(items)


def _sync_editor_controls(
    scene: bpy.types.Scene, selected_material: str | None = None
) -> None:
    global SYNCING_CONTROLS
    document = _active_document(scene)
    if document is None:
        return
    SYNCING_CONTROLS = True
    try:
        selected = selected_material or (
            document.material_names[0] if document.material_names else ""
        )
        if selected not in document.material_names:
            selected = document.material_names[0] if document.material_names else ""
        scene.workout_game_gallery_material = selected
        if selected:
            color, roughness, metallic = _material_values(scene, document, selected)
            scene.workout_game_gallery_color = color
            scene.workout_game_gallery_roughness = roughness
            scene.workout_game_gallery_metallic = metallic
            _set_material_preview(scene, selected, color, roughness, metallic)
        physics = document.physics
        scene.workout_game_gallery_interaction = physics.interaction
        scene.workout_game_gallery_friction = physics.friction
        scene.workout_game_gallery_rolling_resistance = physics.rolling_resistance
        scene.workout_game_gallery_restitution = physics.restitution
        scene.workout_game_gallery_collision_node = physics.collision_node or "NONE"
    finally:
        SYNCING_CONTROLS = False


def _material_selection_changed(scene: bpy.types.Scene, _context=None) -> None:
    if not SYNCING_CONTROLS:
        _sync_editor_controls(scene, scene.workout_game_gallery_material)


def _stage_material_controls(scene: bpy.types.Scene) -> None:
    document = _active_document(scene)
    selected = scene.workout_game_gallery_material
    if document is None or selected not in document.material_names:
        return
    document.set_material(
        selected,
        base_color_srgb=_linear_color_to_hex(scene.workout_game_gallery_color),
        roughness=scene.workout_game_gallery_roughness,
        metallic=scene.workout_game_gallery_metallic,
    )


def _material_preview_changed(scene: bpy.types.Scene, _context=None) -> None:
    if SYNCING_CONTROLS or not EDIT_ENABLED:
        return
    document = _active_document(scene)
    selected = scene.workout_game_gallery_material
    if document is None or selected not in document.material_names:
        return
    _stage_material_controls(scene)
    _set_material_preview(
        scene,
        selected,
        tuple(scene.workout_game_gallery_color),
        scene.workout_game_gallery_roughness,
        scene.workout_game_gallery_metallic,
    )
    scene["workout_game_gallery_save_status"] = (
        f"Unsaved changes for {document.asset_id}"
    )


def _stage_physics_controls(scene: bpy.types.Scene) -> None:
    document = _active_document(scene)
    if document is None:
        return
    document.set_physics(
        interaction=scene.workout_game_gallery_interaction,
        friction=scene.workout_game_gallery_friction,
        rolling_resistance=scene.workout_game_gallery_rolling_resistance,
        restitution=scene.workout_game_gallery_restitution,
        collision_node=(
            "" if scene.workout_game_gallery_collision_node == "NONE"
            else scene.workout_game_gallery_collision_node
        ),
    )


def _physics_changed(scene: bpy.types.Scene, _context=None) -> None:
    global SYNCING_CONTROLS
    if SYNCING_CONTROLS or not EDIT_ENABLED:
        return
    document = _active_document(scene)
    if document is None:
        return
    _stage_physics_controls(scene)
    if (
        document.physics.interaction == "visual-only"
        and scene.workout_game_gallery_collision_node != "NONE"
    ):
        SYNCING_CONTROLS = True
        try:
            scene.workout_game_gallery_collision_node = "NONE"
        finally:
            SYNCING_CONTROLS = False
    scene["workout_game_gallery_save_status"] = (
        f"Unsaved changes for {document.asset_id}"
    )


def _active_collection(collection: bpy.types.Collection) -> None:
    def find(layer: bpy.types.LayerCollection):
        if layer.collection == collection:
            return layer
        for child in layer.children:
            result = find(child)
            if result is not None:
                return result
        return None

    layer = find(bpy.context.view_layer.layer_collection)
    if layer is None:
        raise RuntimeError(f"cannot activate collection {collection.name}")
    bpy.context.view_layer.active_layer_collection = layer


def _import_asset(asset: GalleryAsset) -> ImportedAsset:
    validate_gallery_asset_file(asset)
    collection = bpy.data.collections.new(f"WG Gallery - {asset.display_name}")
    bpy.context.scene.collection.children.link(collection)
    _active_collection(collection)
    before = set(bpy.data.objects)
    bpy.ops.import_scene.gltf(filepath=str(asset.path))

    imported = set(bpy.data.objects) - before
    if asset.asset_id == "RB-01-rider-bike":
        objects = {item.name: item for item in imported}
        assemble_neutral_pose(objects)
        imported = set(bpy.data.objects) - before

    meshes = sorted(item.name for item in imported if item.type == "MESH")
    if not meshes:
        raise RuntimeError(f"asset contains no meshes: {asset.path}")
    document = ASSET_DOCUMENTS.get(asset.asset_id)
    material_map: list[tuple[str, str]] = []
    if document is not None:
        used_materials = {
            slot.material.name
            for item in imported if item.type == "MESH"
            for slot in item.material_slots if slot.material is not None
        }
        for canonical in document.material_names:
            candidates = sorted(
                name for name in used_materials
                if name == canonical or name.startswith(f"{canonical}.")
            )
            if len(candidates) != 1:
                raise RuntimeError(
                    f"cannot map GLB material {asset.asset_id} / {canonical}: "
                    f"{candidates}"
                )
            material_map.append((canonical, candidates[0]))
    return ImportedAsset(
        asset,
        tuple(sorted(item.name for item in imported)),
        tuple(meshes),
        tuple(material_map),
    )


def _clear_scene() -> None:
    for item in list(bpy.data.objects):
        bpy.data.objects.remove(item, do_unlink=True)
    for collection in list(bpy.data.collections):
        bpy.data.collections.remove(collection)


def _asset_index(identifier: str) -> int:
    return gallery_asset_index(
        [imported.catalog for imported in IMPORTED_ASSETS], identifier
    )


def _visible_index(scene: bpy.types.Scene) -> int:
    identifier = scene.workout_game_gallery_asset
    for index, imported in enumerate(IMPORTED_ASSETS):
        if imported.catalog.asset_id == identifier:
            return index
    return 0


def _show_asset(scene: bpy.types.Scene, _context=None) -> None:
    active_index = _visible_index(scene)
    for index, imported in enumerate(IMPORTED_ASSETS):
        for name in imported.object_names:
            item = bpy.data.objects.get(name)
            if item is not None:
                item.hide_set(True)
                item.hide_render = True
                item.select_set(False)
        if index != active_index:
            continue
        for name in imported.mesh_names:
            item = bpy.data.objects.get(name)
            if item is not None:
                item.hide_set(False)
                item.hide_render = False
                item.select_set(True)
    active = IMPORTED_ASSETS[active_index]
    bpy.context.view_layer.objects.active = bpy.data.objects.get(
        active.mesh_names[0]
    )
    scene["workout_game_gallery_details"] = (
        f"{active.catalog.asset_id} | {active.catalog.role} | "
        f"{active.catalog.triangles:,} triangles"
    )
    scene["workout_game_gallery_review_warning"] = (
        "EXTERNAL REVIEW ONLY - never installed in runtime"
        if active.catalog.review_only else ""
    )
    document = ASSET_DOCUMENTS.get(active.catalog.asset_id)
    scene["workout_game_gallery_provenance"] = (
        f"{document.source_summary} | {document.license_id} | "
        f"review: {document.review_status}"
        if document is not None else "External candidate metadata is review-only"
    )
    _sync_editor_controls(scene)
    if not bpy.app.background and not bpy.app.timers.is_registered(
        _frame_all_viewports
    ):
        bpy.app.timers.register(_frame_all_viewports, first_interval=0.01)


def _frame_all_viewports():
    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas:
            if area.type != "VIEW_3D":
                continue
            region = next(
                (item for item in area.regions if item.type == "WINDOW"),
                None,
            )
            if region is None:
                continue
            with bpy.context.temp_override(
                window=window,
                screen=window.screen,
                area=area,
                region=region,
            ):
                bpy.ops.view3d.view_selected(use_all_regions=False)
            area.spaces.active.clip_start = 0.01
            area.spaces.active.clip_end = 1000.0
    return None


def _frame_visible(context: bpy.types.Context) -> None:
    if context.area is None or context.area.type != "VIEW_3D":
        return
    bpy.ops.view3d.view_selected(use_all_regions=False)
    context.space_data.clip_start = 0.01
    context.space_data.clip_end = 1000.0


class WG_GALLERY_OT_step(bpy.types.Operator):
    bl_idname = "wg_gallery.step"
    bl_label = "Select adjacent model"
    bl_options = {"INTERNAL"}

    direction: bpy.props.IntProperty(default=1)

    def execute(self, context):
        index = (_visible_index(context.scene) + self.direction) % len(
            IMPORTED_ASSETS
        )
        context.scene.workout_game_gallery_asset = (
            IMPORTED_ASSETS[index].catalog.asset_id
        )
        _frame_visible(context)
        return {"FINISHED"}


class WG_GALLERY_OT_frame(bpy.types.Operator):
    bl_idname = "wg_gallery.frame"
    bl_label = "Frame model"
    bl_options = {"INTERNAL"}

    def execute(self, context):
        _frame_visible(context)
        return {"FINISHED"}


class WG_GALLERY_OT_view(bpy.types.Operator):
    bl_idname = "wg_gallery.view"
    bl_label = "Set review view"
    bl_options = {"INTERNAL"}

    axis: bpy.props.EnumProperty(
        items=(
            ("FRONT", "Front", ""),
            ("BACK", "Rear", ""),
            ("RIGHT", "Side", ""),
            ("TOP", "Top", ""),
        )
    )

    def execute(self, context):
        bpy.ops.view3d.view_axis(type=self.axis, align_active=False)
        _frame_visible(context)
        return {"FINISHED"}


class WG_GALLERY_OT_save(bpy.types.Operator):
    bl_idname = "wg_gallery.save"
    bl_label = "Save asset metadata"
    bl_description = "Validate and atomically save material and physics metadata"

    def execute(self, context):
        if not EDIT_ENABLED:
            self.report({"ERROR"}, "Gallery was opened read-only")
            return {"CANCELLED"}
        scene = context.scene
        document = _active_document(scene)
        if document is None:
            self.report({"ERROR"}, "External review candidates cannot be edited")
            return {"CANCELLED"}
        try:
            _stage_material_controls(scene)
            _stage_physics_controls(scene)
            dirty = sorted(
                (item for item in ASSET_DOCUMENTS.values() if item.dirty),
                key=lambda item: item.asset_id,
            )
            saved = sum(item.save() for item in dirty)
        except (AssetDocumentError, OSError, RuntimeError) as error:
            self.report({"ERROR"}, str(error))
            return {"CANCELLED"}
        scene["workout_game_gallery_save_status"] = (
            f"Saved {saved} asset document(s); review reset to candidate"
            if saved else "No changes"
        )
        self.report({"INFO"}, scene["workout_game_gallery_save_status"])
        _show_asset(scene)
        return {"FINISHED"}


def _wireframe_changed(scene: bpy.types.Scene, _context=None) -> None:
    mode = "WIREFRAME" if scene.workout_game_gallery_wireframe else "MATERIAL"
    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas:
            if area.type == "VIEW_3D":
                area.spaces.active.shading.type = mode


class WG_GALLERY_PT_models(bpy.types.Panel):
    bl_label = "Workout Game Models"
    bl_idname = "WG_GALLERY_PT_models"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Workout Game"

    def draw(self, context):
        layout = self.layout
        scene = context.scene
        layout.prop(scene, "workout_game_gallery_asset", text="Model")

        row = layout.row(align=True)
        previous = row.operator("wg_gallery.step", text="", icon="TRIA_LEFT")
        previous.direction = -1
        row.operator("wg_gallery.frame", text="", icon="VIEWZOOM")
        next_item = row.operator("wg_gallery.step", text="", icon="TRIA_RIGHT")
        next_item.direction = 1

        row = layout.row(align=True)
        for axis, label in (
            ("FRONT", "Front"),
            ("BACK", "Rear"),
            ("RIGHT", "Side"),
            ("TOP", "Top"),
        ):
            operator = row.operator("wg_gallery.view", text=label)
            operator.axis = axis

        layout.prop(scene, "workout_game_gallery_wireframe", text="Wireframe")
        warning = scene.get("workout_game_gallery_review_warning", "")
        if warning:
            row = layout.row()
            row.alert = True
            row.label(text=warning, icon="ERROR")
        layout.label(text=scene.get("workout_game_gallery_details", ""))
        layout.label(text=scene.get("workout_game_gallery_provenance", ""))
        layout.label(text=scene.get("workout_game_gallery_gpu", ""))

        if EDIT_ENABLED and _active_document(scene) is not None:
            editor = layout.column(align=True)
            editor.separator()
            editor.label(text="Material", icon="MATERIAL")
            editor.prop(scene, "workout_game_gallery_material", text="")
            editor.prop(scene, "workout_game_gallery_color", text="Color")
            editor.prop(scene, "workout_game_gallery_roughness", text="Roughness")
            editor.prop(scene, "workout_game_gallery_metallic", text="Metallic")
            editor.separator()
            editor.label(text="External physics metadata", icon="PHYSICS")
            editor.prop(scene, "workout_game_gallery_interaction", text="Interaction")
            if scene.workout_game_gallery_interaction != "visual-only":
                editor.prop(scene, "workout_game_gallery_friction", text="Friction")
                editor.prop(
                    scene,
                    "workout_game_gallery_rolling_resistance",
                    text="Rolling resistance",
                )
                editor.prop(scene, "workout_game_gallery_restitution", text="Restitution")
            editor.prop(scene, "workout_game_gallery_collision_node", text="Proxy")
            editor.operator("wg_gallery.save", text="Save", icon="FILE_TICK")
            status = scene.get("workout_game_gallery_save_status", "")
            if status:
                editor.label(text=status)
        else:
            layout.label(text="Read-only review", icon="LOCKED")


CLASSES = (
    WG_GALLERY_OT_step,
    WG_GALLERY_OT_frame,
    WG_GALLERY_OT_view,
    WG_GALLERY_OT_save,
    WG_GALLERY_PT_models,
)


def _configure_ui(initial_index: int) -> None:
    for cls in CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.Scene.workout_game_gallery_asset = bpy.props.EnumProperty(
        name="Model",
        items=[
            (
                imported.catalog.asset_id,
                imported.catalog.display_name,
                imported.catalog.role,
            )
            for imported in IMPORTED_ASSETS
        ],
        update=_show_asset,
    )
    bpy.types.Scene.workout_game_gallery_wireframe = bpy.props.BoolProperty(
        name="Wireframe",
        default=False,
        update=_wireframe_changed,
    )
    bpy.types.Scene.workout_game_gallery_material = bpy.props.EnumProperty(
        name="Material",
        items=_material_items,
        update=_material_selection_changed,
    )
    bpy.types.Scene.workout_game_gallery_color = bpy.props.FloatVectorProperty(
        name="Color",
        subtype="COLOR",
        size=3,
        min=0.0,
        max=1.0,
        default=(0.8, 0.8, 0.8),
        update=_material_preview_changed,
    )
    bpy.types.Scene.workout_game_gallery_roughness = bpy.props.FloatProperty(
        name="Roughness",
        min=0.0,
        max=1.0,
        default=1.0,
        update=_material_preview_changed,
    )
    bpy.types.Scene.workout_game_gallery_metallic = bpy.props.FloatProperty(
        name="Metallic",
        min=0.0,
        max=1.0,
        default=0.0,
        update=_material_preview_changed,
    )
    bpy.types.Scene.workout_game_gallery_interaction = bpy.props.EnumProperty(
        name="Interaction",
        items=(
            ("visual-only", "Visual only", "No runtime contact surface"),
            ("surface", "Surface", "Rideable terrain surface"),
            ("obstacle", "Obstacle", "External obstacle contact"),
            ("rideable-feature", "Rideable feature", "External feature physics"),
        ),
        default="visual-only",
        update=_physics_changed,
    )
    bpy.types.Scene.workout_game_gallery_friction = bpy.props.FloatProperty(
        name="Friction", min=0.0, max=2.0, default=1.0,
        update=_physics_changed,
    )
    bpy.types.Scene.workout_game_gallery_rolling_resistance = bpy.props.FloatProperty(
        name="Rolling resistance", min=0.0, max=0.1, default=0.0, precision=3,
        update=_physics_changed,
    )
    bpy.types.Scene.workout_game_gallery_restitution = bpy.props.FloatProperty(
        name="Restitution", min=0.0, max=0.25, default=0.0, precision=3,
        update=_physics_changed,
    )
    bpy.types.Scene.workout_game_gallery_collision_node = bpy.props.EnumProperty(
        name="Collision proxy",
        items=_collision_node_items,
        update=_physics_changed,
    )
    bpy.context.scene.workout_game_gallery_asset = (
        IMPORTED_ASSETS[initial_index].catalog.asset_id
    )
    _show_asset(bpy.context.scene)

    for area in bpy.context.screen.areas if bpy.context.screen else []:
        if area.type == "VIEW_3D":
            area.spaces.active.shading.type = "MATERIAL"
            area.spaces.active.show_region_ui = True
            area.spaces.active.overlay.show_floor = True
            area.spaces.active.overlay.show_axis_x = True
            area.spaces.active.overlay.show_axis_y = True


def _run_ui_smoke_test():
    windows = bpy.context.window_manager.windows
    if not windows:
        raise RuntimeError("UI smoke test has no Blender window")
    window = windows[0]
    area = next(
        (item for item in window.screen.areas if item.type == "VIEW_3D"),
        None,
    )
    if area is None:
        raise RuntimeError("UI smoke test has no 3D viewport")
    region = next(
        (item for item in area.regions if item.type == "WINDOW"),
        None,
    )
    if region is None:
        raise RuntimeError("UI smoke test has no viewport window region")

    scene = bpy.context.scene
    selected_asset = scene.workout_game_gallery_asset
    scene.workout_game_gallery_asset = IMPORTED_ASSETS[-1].catalog.asset_id
    scene.workout_game_gallery_asset = selected_asset
    with bpy.context.temp_override(
        window=window,
        screen=window.screen,
        area=area,
        region=region,
    ):
        bpy.ops.view3d.view_axis(type="RIGHT", align_active=False)
        bpy.ops.view3d.view_selected(use_all_regions=False)
    scene.workout_game_gallery_wireframe = True
    scene.workout_game_gallery_wireframe = False
    area.spaces.active.show_region_ui = True
    area.tag_redraw()
    bpy.context.view_layer.update()
    with bpy.context.temp_override(window=window, screen=window.screen):
        bpy.ops.wm.redraw_timer(type="DRAW_WIN_SWAP", iterations=3)

    if GPU_EVIDENCE_PATH is not None:
        details = _record_gpu_evidence(GPU_EVIDENCE_PATH)
        print(f"Workout Game gallery GPU evidence: {details}", flush=True)
    if UI_SCREENSHOT_PATH is not None:
        UI_SCREENSHOT_PATH.parent.mkdir(parents=True, exist_ok=True)
        with bpy.context.temp_override(window=window, screen=window.screen):
            result = bpy.ops.screen.screenshot(filepath=str(UI_SCREENSHOT_PATH))
        if "FINISHED" not in result or not UI_SCREENSHOT_PATH.is_file():
            raise RuntimeError("gallery UI screenshot failed")
        _validate_ui_screenshot(UI_SCREENSHOT_PATH)

    print("Workout Game gallery UI smoke test passed", flush=True)
    bpy.ops.wm.quit_blender()
    return None


def _run_editor_smoke_test() -> None:
    if not EDIT_ENABLED or REPOSITORY is None:
        raise RuntimeError("editor smoke test requires --edit")
    scene = bpy.context.scene
    document = _active_document(scene)
    if document is None or len(document.material_names) < 2:
        raise RuntimeError("editor smoke test needs two editable materials")
    expected_values: dict[str, float] = {}
    for index, material_name in enumerate(document.material_names[:2]):
        scene.workout_game_gallery_material = material_name
        previous = scene.workout_game_gallery_roughness
        candidate = 0.37 + index * 0.11
        expected = candidate if not math.isclose(
            previous, candidate, abs_tol=1.0e-6
        ) else candidate + 0.19
        scene.workout_game_gallery_roughness = expected
        expected_values[material_name] = expected
    other_asset = next(
        imported.catalog.asset_id
        for imported in IMPORTED_ASSETS
        if imported.catalog.asset_id != document.asset_id
    )
    scene.workout_game_gallery_asset = other_asset
    scene.workout_game_gallery_asset = document.asset_id
    first_material = next(iter(expected_values))
    scene.workout_game_gallery_material = first_material
    if not math.isclose(
        scene.workout_game_gallery_roughness,
        expected_values[first_material],
        abs_tol=1.0e-6,
    ):
        raise RuntimeError("editor smoke lost an edit across asset selection")
    result = bpy.ops.wg_gallery.save()
    if "FINISHED" not in result:
        raise RuntimeError(f"editor smoke save failed: {result}")
    reopened = AssetDocument.open(REPOSITORY, document.asset_id)
    for material_name, expected in expected_values.items():
        actual = reopened.material_overrides[material_name]["roughness"]
        if not math.isclose(actual, expected, abs_tol=1.0e-6):
            raise RuntimeError(
                f"editor smoke reload mismatch for {material_name}: "
                f"expected {expected}, got {actual}"
            )
    print(
        f"Workout Game gallery editor smoke test passed: "
        f"{document.asset_id} / {', '.join(expected_values)}",
        flush=True,
    )


def _run_catalog_smoke_test() -> None:
    scene = bpy.context.scene
    for imported in IMPORTED_ASSETS:
        scene.workout_game_gallery_asset = imported.catalog.asset_id
        document = _active_document(scene)
        if document is None:
            continue
        for material_name in document.material_names:
            scene.workout_game_gallery_material = material_name
            _material_values(scene, document, material_name)
            material = _material(scene, material_name)
            visible_materials = {
                slot.material
                for object_name in imported.mesh_names
                for slot in bpy.data.objects[object_name].material_slots
                if slot.material is not None
            }
            if material not in visible_materials:
                raise RuntimeError(
                    f"material mapping escaped active asset: "
                    f"{imported.catalog.asset_id} / {material_name}"
                )


def main() -> None:
    global EDIT_ENABLED, GPU_EVIDENCE_PATH, REPOSITORY, UI_SCREENSHOT_PATH
    arguments = _arguments()
    repository = Path(arguments.root).expanduser().resolve()
    REPOSITORY = repository
    EDIT_ENABLED = arguments.edit
    if arguments.evidence_directory and (
        arguments.gpu_evidence or arguments.ui_screenshot
    ):
        raise RuntimeError(
            "--evidence-directory cannot be combined with individual evidence paths"
        )
    evidence_directory = (
        Path(arguments.evidence_directory) if arguments.evidence_directory else None
    )
    GPU_EVIDENCE_PATH = (
        evidence_directory / "gpu.json"
        if evidence_directory is not None
        else Path(arguments.gpu_evidence) if arguments.gpu_evidence else None
    )
    UI_SCREENSHOT_PATH = (
        evidence_directory / "gallery.png"
        if evidence_directory is not None
        else Path(arguments.ui_screenshot) if arguments.ui_screenshot else None
    )
    catalog = load_gallery_assets(repository)
    if arguments.candidate_directory:
        catalog.extend(load_candidate_gallery_assets(
            repository,
            Path(arguments.candidate_directory),
        ))
    if not catalog:
        raise RuntimeError("no manifest-backed GLB assets found")

    ASSET_DOCUMENTS.update({
        asset.asset_id: AssetDocument.open(repository, asset.asset_id)
        for asset in catalog if not asset.review_only
    })

    _clear_scene()
    IMPORTED_ASSETS.extend(_import_asset(asset) for asset in catalog)
    initial_index = _asset_index(arguments.asset)
    _configure_ui(initial_index)
    gpu_details = _gpu_details()
    bpy.context.scene["workout_game_gallery_gpu"] = (
        f"{gpu_details['backend']} | {gpu_details['vendor']} | "
        f"{gpu_details['renderer']}"
    )
    if GPU_EVIDENCE_PATH is not None and not arguments.ui_smoke_test:
        _record_gpu_evidence(GPU_EVIDENCE_PATH)

    print(
        "Workout Game gallery loaded:",
        ", ".join(item.catalog.asset_id for item in IMPORTED_ASSETS),
    )
    if arguments.smoke_test:
        _run_catalog_smoke_test()
        print(
            f"Workout Game gallery smoke test passed: "
            f"{len(IMPORTED_ASSETS)} assets"
        )
    if arguments.editor_smoke_test:
        _run_editor_smoke_test()
    if arguments.ui_smoke_test:
        if bpy.app.background:
            raise RuntimeError("UI smoke test requires a graphical Blender window")
        bpy.app.timers.register(
            _run_ui_smoke_test,
            first_interval=1.0,
        )


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"ERROR: {error}", file=sys.stderr)
        traceback.print_exc()
        raise SystemExit(1)
