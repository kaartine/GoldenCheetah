import QtQuick
import QtQuick3D

Node {
    id: root
    objectName: "workoutGameForestVergeCluster"
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
        source: "assets/meshes/geo_VergeGraniteBilberry_LOD0_mesh.mesh"
        materials: [graniteMaterial, understoryMaterial]
        castsShadows: false
        receivesShadows: false
    }
    Model {
        visible: root.variant === 1
        source: "assets/meshes/geo_VergeStumpFern_LOD0_mesh.mesh"
        materials: [graniteMaterial, barkMaterial,
                    endGrainMaterial, understoryMaterial]
        castsShadows: false
        receivesShadows: false
    }
    Model {
        visible: root.variant === 2
        source: "assets/meshes/geo_VergeDeadwoodHeather_LOD0_mesh.mesh"
        materials: [barkMaterial, endGrainMaterial, understoryMaterial]
        castsShadows: false
        receivesShadows: false
    }
}
