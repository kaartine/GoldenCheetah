import QtQuick
import QtQuick3D

Node {
    id: root
    required property int variant
    readonly property bool scotsPine: variant === 3

    PrincipledMaterial {
        id: barkMaterial
        baseColor: "#56371f"
        roughness: 1
    }
    PrincipledMaterial {
        id: pineBarkMaterial
        baseColor: "#a35f2f"
        roughness: 1
    }
    PrincipledMaterial {
        id: darkFoliageMaterial
        baseColor: "#1d5a33"
        roughness: 1
    }
    PrincipledMaterial {
        id: lightFoliageMaterial
        baseColor: root.scotsPine ? "#285f37"
                   : root.variant === 1 ? "#347343" : "#2a663b"
        roughness: 1
    }

    Model {
        visible: !root.scotsPine
        source: "assets/meshes/geo_ConiferTrunk_LOD0_mesh.mesh"
        materials: barkMaterial
        castsShadows: false
        receivesShadows: false
    }
    Model {
        visible: root.variant === 0
        source: "assets/meshes/geo_ConiferNarrow_LOD0_mesh.mesh"
        materials: darkFoliageMaterial
        castsShadows: false
        receivesShadows: false
    }
    Model {
        visible: root.variant === 1
        source: "assets/meshes/geo_ConiferLayered_LOD0_mesh.mesh"
        materials: lightFoliageMaterial
        castsShadows: false
        receivesShadows: false
    }
    Model {
        visible: root.variant === 2
        source: "assets/meshes/geo_ConiferBrokenTop_LOD0_mesh.mesh"
        materials: darkFoliageMaterial
        castsShadows: false
        receivesShadows: false
    }
    Model {
        objectName: "workoutGameScotsPineTrunk"
        visible: root.scotsPine
        source: "assets/meshes/geo_ScotsPineTrunk_LOD0_mesh.mesh"
        materials: pineBarkMaterial
        castsShadows: false
        receivesShadows: false
    }
    Model {
        objectName: "workoutGameScotsPineCrown"
        visible: root.scotsPine
        source: "assets/meshes/geo_ScotsPineCrown_LOD0_mesh.mesh"
        materials: lightFoliageMaterial
        castsShadows: false
        receivesShadows: false
    }
}
