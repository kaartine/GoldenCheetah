import QtQuick
import QtQuick3D

Node {
    id: root
    required property int variant

    PrincipledMaterial {
        id: barkMaterial
        baseColor: "#56371f"
        roughness: 1
    }
    PrincipledMaterial {
        id: darkFoliageMaterial
        baseColor: root.variant === 3 ? "#17482b" : "#1d5a33"
        roughness: 1
    }
    PrincipledMaterial {
        id: lightFoliageMaterial
        baseColor: root.variant === 1 ? "#347343" : "#2a663b"
        roughness: 1
    }

    Model {
        source: "assets/meshes/geo_ConiferTrunk_LOD0_mesh.mesh"
        materials: barkMaterial
        castsShadows: false
        receivesShadows: false
    }
    Model {
        visible: root.variant % 3 === 0
        source: "assets/meshes/geo_ConiferNarrow_LOD0_mesh.mesh"
        materials: darkFoliageMaterial
        castsShadows: false
        receivesShadows: false
    }
    Model {
        visible: root.variant % 3 === 1
        source: "assets/meshes/geo_ConiferLayered_LOD0_mesh.mesh"
        materials: lightFoliageMaterial
        castsShadows: false
        receivesShadows: false
    }
    Model {
        visible: root.variant % 3 === 2
        source: "assets/meshes/geo_ConiferBrokenTop_LOD0_mesh.mesh"
        materials: darkFoliageMaterial
        castsShadows: false
        receivesShadows: false
    }
}
