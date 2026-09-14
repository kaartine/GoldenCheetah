import QtQuick
import QtQuick3D

Node {
    id: root
    required property int variant
    property var instanceTable: null
    readonly property bool scotsPine: variant === 3
    readonly property bool birch: variant === 2

    PrincipledMaterial {
        id: barkMaterial
        baseColor: "#56371f"
        roughness: 1
        alphaMode: PrincipledMaterial.Blend
    }
    PrincipledMaterial {
        id: pineBarkMaterial
        baseColor: "#a35f2f"
        roughness: 1
        alphaMode: PrincipledMaterial.Blend
    }
    PrincipledMaterial {
        id: birchBarkMaterial
        baseColor: "#c2c2a8"
        roughness: 1
        alphaMode: PrincipledMaterial.Blend
    }
    PrincipledMaterial {
        id: darkFoliageMaterial
        baseColor: "#1d5a33"
        roughness: 1
        alphaMode: PrincipledMaterial.Blend
    }
    PrincipledMaterial {
        id: lightFoliageMaterial
        baseColor: root.scotsPine ? "#285f37"
                   : root.variant === 1 ? "#347343" : "#2a663b"
        roughness: 1
        alphaMode: PrincipledMaterial.Blend
    }
    PrincipledMaterial {
        id: birchFoliageMaterial
        baseColor: "#397a37"
        roughness: 1
        alphaMode: PrincipledMaterial.Blend
    }

    Model {
        visible: !root.scotsPine && !root.birch
        source: "assets/meshes/geo_ConiferTrunk_LOD0_mesh.mesh"
        instancing: root.instanceTable
        materials: barkMaterial
        castsShadows: false
        receivesShadows: false
    }
    Model {
        visible: root.variant === 0
        source: "assets/meshes/geo_ConiferNarrow_LOD0_mesh.mesh"
        instancing: root.instanceTable
        materials: darkFoliageMaterial
        castsShadows: false
        receivesShadows: false
    }
    Model {
        visible: root.variant === 1
        source: "assets/meshes/geo_ConiferLayered_LOD0_mesh.mesh"
        instancing: root.instanceTable
        materials: lightFoliageMaterial
        castsShadows: false
        receivesShadows: false
    }
    Model {
        objectName: "workoutGameScotsPineTrunk"
        visible: root.scotsPine
        source: "assets/meshes/geo_ScotsPineTrunk_LOD0_mesh.mesh"
        instancing: root.instanceTable
        materials: pineBarkMaterial
        castsShadows: false
        receivesShadows: false
    }
    Model {
        objectName: "workoutGameScotsPineCrown"
        visible: root.scotsPine
        source: "assets/meshes/geo_ScotsPineCrown_LOD0_mesh.mesh"
        instancing: root.instanceTable
        materials: lightFoliageMaterial
        castsShadows: false
        receivesShadows: false
    }
    Model {
        objectName: "workoutGameBirchTrunk"
        visible: root.birch
        source: "assets/meshes/geo_BirchTrunk_LOD0_mesh.mesh"
        instancing: root.instanceTable
        materials: birchBarkMaterial
        castsShadows: false
        receivesShadows: false
    }
    Model {
        objectName: "workoutGameBirchCrown"
        visible: root.birch
        source: "assets/meshes/geo_BirchCrown_LOD0_mesh.mesh"
        instancing: root.instanceTable
        materials: birchFoliageMaterial
        castsShadows: false
        receivesShadows: false
    }
}
