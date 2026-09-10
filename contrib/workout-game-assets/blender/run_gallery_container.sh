#!/usr/bin/env bash
set -euo pipefail

repository="${WG_GALLERY_ROOT:-/work}"
config="${BLENDER_USER_CONFIG:-/tmp/gc-blender-config}"
export BLENDER_USER_CONFIG="$config"
mkdir -p "$config"

if [[ ! -s "$config/userpref.blend" ]]; then
    blender --background --factory-startup --python-expr \
        'import bpy; bpy.context.preferences.view.show_splash = False; bpy.ops.wm.save_userpref()'
fi

exec blender \
    --python "$repository/contrib/workout-game-assets/blender/workout_game_asset_gallery.py" \
    -- --root "$repository" "$@"
