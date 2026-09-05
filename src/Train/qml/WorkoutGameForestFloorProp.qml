import QtQuick
import QtQuick3D

Node {
    id: root
    objectName: "workoutGameForestFloorProp"
    required property int variant

    PrincipledMaterial {
        id: graniteMaterial
        baseColor: "#1f2426"
        roughness: 1
        cullMode: Material.NoCulling
    }
    PrincipledMaterial {
        id: barkMaterial
        baseColor: "#402411"
        roughness: 1
        cullMode: Material.NoCulling
    }
    PrincipledMaterial {
        id: endGrainMaterial
        baseColor: "#6b4a26"
        roughness: 1
        cullMode: Material.NoCulling
    }
    PrincipledMaterial {
        id: understoryMaterial
        baseColor: "#1f4a21"
        roughness: 1
        cullMode: Material.NoCulling
    }

    Model {
        visible: root.variant === 0
        source: "assets/meshes/geo_GraniteLow_LOD0_mesh.mesh"
        materials: graniteMaterial
        castsShadows: false
        receivesShadows: false
    }
    Model {
        visible: root.variant === 1
        source: "assets/meshes/geo_GraniteUpright_LOD0_mesh.mesh"
        materials: graniteMaterial
        castsShadows: false
        receivesShadows: false
    }
    Model {
        visible: root.variant === 2
        source: "assets/meshes/geo_GraniteSlab_LOD0_mesh.mesh"
        materials: graniteMaterial
        castsShadows: false
        receivesShadows: false
    }
    Model {
        visible: root.variant === 3
        source: "assets/meshes/geo_StumpRooted_LOD0_mesh.mesh"
        materials: [barkMaterial, endGrainMaterial]
        castsShadows: false
        receivesShadows: false
    }
    Model {
        visible: root.variant === 4
        source: "assets/meshes/geo_DeadwoodFallen_LOD0_mesh.mesh"
        materials: [barkMaterial, endGrainMaterial]
        castsShadows: false
        receivesShadows: false
    }
    Model {
        visible: root.variant === 5
        source: "assets/meshes/geo_UnderstoryFern_LOD0_mesh.mesh"
        materials: understoryMaterial
        castsShadows: false
        receivesShadows: false
    }
    Model {
        visible: root.variant === 6
        source: "assets/meshes/geo_UnderstoryBilberry_LOD0_mesh.mesh"
        materials: understoryMaterial
        castsShadows: false
        receivesShadows: false
    }
    Model {
        visible: root.variant === 7
        source: "assets/meshes/geo_UnderstoryHeather_LOD0_mesh.mesh"
        materials: understoryMaterial
        castsShadows: false
        receivesShadows: false
    }
}
