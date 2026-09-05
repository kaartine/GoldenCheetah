import QtQuick
import QtQuick3D

Node {
    id: root
    objectName: "workoutGameForestFloorProp"
    required property int variant

    PrincipledMaterial {
        id: graniteMaterial
        baseColor: "#52636b"
        roughness: 1
        cullMode: Material.NoCulling
    }
    PrincipledMaterial {
        id: barkMaterial
        baseColor: "#6b3d1f"
        roughness: 1
        cullMode: Material.NoCulling
    }
    PrincipledMaterial {
        id: endGrainMaterial
        baseColor: "#d18b3f"
        roughness: 1
        cullMode: Material.NoCulling
    }
    PrincipledMaterial {
        id: mossMaterial
        baseColor: "#4e8135"
        roughness: 1
        cullMode: Material.NoCulling
    }
    PrincipledMaterial {
        id: fernMaterial
        baseColor: "#45a34b"
        roughness: 1
        cullMode: Material.NoCulling
    }
    PrincipledMaterial {
        id: bilberryMaterial
        baseColor: "#296b4a"
        roughness: 1
        cullMode: Material.NoCulling
    }
    PrincipledMaterial {
        id: heatherMaterial
        baseColor: "#87567f"
        roughness: 1
        cullMode: Material.NoCulling
    }

    Model {
        visible: root.variant === 0
        source: "assets/meshes/geo_GraniteLow_LOD0_mesh.mesh"
        materials: [graniteMaterial, mossMaterial]
        castsShadows: false
        receivesShadows: false
    }
    Model {
        visible: root.variant === 1
        source: "assets/meshes/geo_GraniteUpright_LOD0_mesh.mesh"
        materials: [graniteMaterial, mossMaterial]
        castsShadows: false
        receivesShadows: false
    }
    Model {
        visible: root.variant === 2
        source: "assets/meshes/geo_GraniteSlab_LOD0_mesh.mesh"
        materials: [graniteMaterial, mossMaterial]
        castsShadows: false
        receivesShadows: false
    }
    Model {
        visible: root.variant === 3
        source: "assets/meshes/geo_StumpRooted_LOD0_mesh.mesh"
        materials: [barkMaterial, endGrainMaterial, mossMaterial]
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
        materials: fernMaterial
        castsShadows: false
        receivesShadows: false
    }
    Model {
        visible: root.variant === 6
        source: "assets/meshes/geo_UnderstoryBilberry_LOD0_mesh.mesh"
        materials: bilberryMaterial
        castsShadows: false
        receivesShadows: false
    }
    Model {
        visible: root.variant === 7
        source: "assets/meshes/geo_UnderstoryHeather_LOD0_mesh.mesh"
        materials: heatherMaterial
        castsShadows: false
        receivesShadows: false
    }
}
