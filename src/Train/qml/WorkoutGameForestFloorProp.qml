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
    PrincipledMaterial {
        id: shrubMaterial
        baseColor: "#326747"
        roughness: 1
        cullMode: Material.NoCulling
    }
    PrincipledMaterial {
        id: grassMaterial
        baseColor: "#5e913d"
        roughness: 1
        cullMode: Material.NoCulling
    }
    PrincipledMaterial {
        id: flowerMaterial
        baseColor: "#d7a4d9"
        roughness: 1
        cullMode: Material.NoCulling
    }
    PrincipledMaterial {
        id: mushroomStalkMaterial
        baseColor: "#d9c7a5"
        roughness: 1
        cullMode: Material.NoCulling
    }
    PrincipledMaterial {
        id: mushroomCapMaterial
        baseColor: "#b84d3f"
        roughness: 1
        cullMode: Material.NoCulling
    }
    PrincipledMaterial {
        id: pineNeedleMaterial
        baseColor: "#24583a"
        roughness: 1
        cullMode: Material.NoCulling
    }
    PrincipledMaterial {
        id: leafySaplingMaterial
        baseColor: "#4a7f3d"
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
    Model {
        visible: root.variant === 8
        source: "assets/meshes/geo_GranitePair_LOD0_mesh.mesh"
        materials: [graniteMaterial, mossMaterial]
        castsShadows: false
        receivesShadows: false
    }
    Model {
        visible: root.variant === 9
        source: "assets/meshes/geo_UnderstoryShrub_LOD0_mesh.mesh"
        materials: shrubMaterial
        castsShadows: false
        receivesShadows: false
    }
    Model {
        visible: root.variant === 10
        source: "assets/meshes/geo_UnderstoryGrass_LOD0_mesh.mesh"
        materials: grassMaterial
        castsShadows: false
        receivesShadows: false
    }
    Model {
        visible: root.variant === 11
        source: "assets/meshes/geo_WildflowerPatch_LOD0_mesh.mesh"
        materials: [grassMaterial, flowerMaterial]
        castsShadows: false
        receivesShadows: false
    }
    Model {
        visible: root.variant === 12
        source: "assets/meshes/geo_MushroomCluster_LOD0_mesh.mesh"
        materials: [mushroomStalkMaterial, mushroomCapMaterial]
        castsShadows: false
        receivesShadows: false
    }
    Model {
        visible: root.variant === 13
        source: "assets/meshes/geo_TwigPile_LOD0_mesh.mesh"
        materials: [barkMaterial, endGrainMaterial]
        castsShadows: false
        receivesShadows: false
    }
    Model {
        visible: root.variant === 14
        source: "assets/meshes/geo_PineSapling_LOD0_mesh.mesh"
        materials: [barkMaterial, pineNeedleMaterial]
        castsShadows: false
        receivesShadows: false
    }
    Model {
        visible: root.variant === 15
        source: "assets/meshes/geo_LeafySapling_LOD0_mesh.mesh"
        materials: [barkMaterial, leafySaplingMaterial]
        castsShadows: false
        receivesShadows: false
    }
}
