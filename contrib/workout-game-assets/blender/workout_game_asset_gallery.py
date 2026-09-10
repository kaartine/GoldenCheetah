#!/usr/bin/env python3
"""Open the committed Workout Game GLBs in a small Blender review gallery."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import sys
import traceback

import bpy


SCRIPT_DIRECTORY = Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIRECTORY))
sys.path.insert(0, str(SCRIPT_DIRECTORY.parent))

from gallery_catalog import GalleryAsset, load_gallery_assets  # noqa: E402
from render_rider_bike_audit import assemble_neutral_pose  # noqa: E402


@dataclass
class ImportedAsset:
    catalog: GalleryAsset
    object_names: tuple[str, ...]
    mesh_names: tuple[str, ...]


IMPORTED_ASSETS: list[ImportedAsset] = []


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
    parser.add_argument("--smoke-test", action="store_true")
    parser.add_argument("--ui-smoke-test", action="store_true")
    return parser.parse_args(arguments)


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
    return ImportedAsset(
        asset,
        tuple(sorted(item.name for item in imported)),
        tuple(meshes),
    )


def _clear_scene() -> None:
    for item in list(bpy.data.objects):
        bpy.data.objects.remove(item, do_unlink=True)
    for collection in list(bpy.data.collections):
        bpy.data.collections.remove(collection)


def _asset_index(identifier: str) -> int:
    normalized = identifier.casefold()
    for index, imported in enumerate(IMPORTED_ASSETS):
        candidates = {
            imported.catalog.asset_id.casefold(),
            imported.catalog.display_name.casefold(),
            imported.catalog.path.stem.casefold(),
        }
        if normalized in candidates:
            return index
    return 0


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
        layout.label(text=scene.get("workout_game_gallery_details", ""))


CLASSES = (
    WG_GALLERY_OT_step,
    WG_GALLERY_OT_frame,
    WG_GALLERY_OT_view,
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
    scene.workout_game_gallery_asset = IMPORTED_ASSETS[-1].catalog.asset_id
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

    print("Workout Game gallery UI smoke test passed", flush=True)
    bpy.ops.wm.quit_blender()
    return None


def main() -> None:
    arguments = _arguments()
    repository = Path(arguments.root).expanduser().resolve()
    catalog = load_gallery_assets(repository)
    if not catalog:
        raise RuntimeError("no manifest-backed GLB assets found")

    _clear_scene()
    IMPORTED_ASSETS.extend(_import_asset(asset) for asset in catalog)
    initial_index = _asset_index(arguments.asset)
    _configure_ui(initial_index)

    print(
        "Workout Game gallery loaded:",
        ", ".join(item.catalog.asset_id for item in IMPORTED_ASSETS),
    )
    if arguments.smoke_test:
        print(
            f"Workout Game gallery smoke test passed: "
            f"{len(IMPORTED_ASSETS)} assets"
        )
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
