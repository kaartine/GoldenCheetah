import QtQuick
import QtQuick3D

Node {
    id: root
    objectName: "workoutGameForestFloorProp"
    required property int variant
    required property int biome
    property var instanceTable: null
    property bool transparent: false

    PrincipledMaterial {
        id: vertexColorMaterial
        baseColor: "white"
        alphaMode: root.transparent
                   ? PrincipledMaterial.Blend : PrincipledMaterial.Opaque
        vertexColorsEnabled: true
        roughness: 1
        cullMode: Material.NoCulling
    }

    readonly property var meshSources: [
        "assets/meshes/geo_GraniteLow_LOD0_mesh.mesh",
        "assets/meshes/geo_GraniteUpright_LOD0_mesh.mesh",
        "assets/meshes/geo_GraniteSlab_LOD0_mesh.mesh",
        "assets/meshes/geo_StumpRooted_LOD0_mesh.mesh",
        "assets/meshes/geo_DeadwoodFallen_LOD0_mesh.mesh",
        "assets/meshes/geo_UnderstoryFern_LOD0_mesh.mesh",
        "assets/meshes/geo_UnderstoryBilberry_LOD0_mesh.mesh",
        "assets/meshes/geo_UnderstoryHeather_LOD0_mesh.mesh",
        "assets/meshes/geo_GranitePair_LOD0_mesh.mesh",
        "assets/meshes/geo_UnderstoryShrub_LOD0_mesh.mesh",
        "assets/meshes/geo_UnderstoryGrass_LOD0_mesh.mesh",
        "assets/meshes/geo_WildflowerPatch_LOD0_mesh.mesh",
        "assets/meshes/geo_MushroomCluster_LOD0_mesh.mesh",
        "assets/meshes/geo_TwigPile_LOD0_mesh.mesh",
        "assets/meshes/geo_PineSapling_LOD0_mesh.mesh",
        "assets/meshes/geo_LeafySapling_LOD0_mesh.mesh"
    ]
    Model {
        source: root.meshSources[root.variant]
        materials: vertexColorMaterial
        instancing: root.instanceTable
        castsShadows: false
        receivesShadows: false
    }
}
