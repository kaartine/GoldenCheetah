import QtQuick
import QtQuick3D

Node {
    id: root
    objectName: "workoutGameForestVergeCluster"
    required property int variant
    required property int biome
    property var instanceTable: null
    property bool transparent: false

    WorkoutGameDevelopmentAsset {
        id: development
        assetId: "EN-09-forest-verge-clusters"
        variantKey: String(root.variant)
        instanceTable: root.instanceTable
        allowed: !root.transparent
    }

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
        "assets/meshes/geo_VergeGraniteBilberry_LOD0_mesh.mesh",
        "assets/meshes/geo_VergeStumpFern_LOD0_mesh.mesh",
        "assets/meshes/geo_VergeDeadwoodHeather_LOD0_mesh.mesh",
        "assets/meshes/geo_VergeRockGrass_LOD0_mesh.mesh",
        "assets/meshes/geo_VergeShrubFlowers_LOD0_mesh.mesh",
        "assets/meshes/geo_VergeSaplingMushroom_LOD0_mesh.mesh"
    ]
    Model {
        visible: !development.ready
        source: root.meshSources[root.variant]
        materials: vertexColorMaterial
        instancing: root.instanceTable
        castsShadows: false
        receivesShadows: false
    }
}
