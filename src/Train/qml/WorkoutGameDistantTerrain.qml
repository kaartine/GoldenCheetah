import QtQuick3D

Node {
    id: root
    objectName: "distantTerrainNode"

    required property var surfaceTexture

    Model {
        objectName: "distantRidgeModel"
        source: "assets/meshes/geo_DistantRidges_LOD0_mesh.mesh"
        materials: PrincipledMaterial {
            baseColor: "#365e43"
            baseColorMap: root.surfaceTexture
            roughness: 1
            lighting: PrincipledMaterial.FragmentLighting
            cullMode: Material.NoCulling
        }
        castsShadows: false
        receivesShadows: false
    }
}
