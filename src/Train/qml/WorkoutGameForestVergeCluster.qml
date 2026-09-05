import QtQuick
import QtQuick3D

Node {
    id: root
    objectName: "workoutGameForestVergeCluster"
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
        id: bilberryMaterial
        baseColor: "#296b4a"
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
        id: heatherMaterial
        baseColor: "#87567f"
        roughness: 1
        cullMode: Material.NoCulling
    }

    Model {
        visible: root.variant === 0
        source: "assets/meshes/geo_VergeGraniteBilberry_LOD0_mesh.mesh"
        materials: [graniteMaterial, bilberryMaterial]
        castsShadows: false
        receivesShadows: false
    }
    Model {
        visible: root.variant === 1
        source: "assets/meshes/geo_VergeStumpFern_LOD0_mesh.mesh"
        materials: [graniteMaterial, barkMaterial,
                    endGrainMaterial, fernMaterial]
        castsShadows: false
        receivesShadows: false
    }
    Model {
        visible: root.variant === 2
        source: "assets/meshes/geo_VergeDeadwoodHeather_LOD0_mesh.mesh"
        materials: [barkMaterial, endGrainMaterial, heatherMaterial]
        castsShadows: false
        receivesShadows: false
    }
}
