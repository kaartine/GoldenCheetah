import QtQuick
import QtQuick3D

Node {
    id: node

    // Resources
    PrincipledMaterial {
        id: mat_LogOverBark_Grey_material
        objectName: "MAT_LogOverBark_Grey"
        baseColor: "#ff47240e"
        roughness: 1
        cullMode: PrincipledMaterial.NoCulling
        alphaMode: PrincipledMaterial.Opaque
    }
    PrincipledMaterial {
        id: mat_LogOverEndGrain_Grey_material
        objectName: "MAT_LogOverEndGrain_Grey"
        baseColor: "#ff855221"
        roughness: 1
        cullMode: PrincipledMaterial.NoCulling
        alphaMode: PrincipledMaterial.Opaque
    }
    PrincipledMaterial {
        id: mat_LogOverBypass_Grey_material
        objectName: "MAT_LogOverBypass_Grey"
        baseColor: "#ff785729"
        roughness: 1
        cullMode: PrincipledMaterial.NoCulling
        alphaMode: PrincipledMaterial.Opaque
    }

    // Nodes:
    Node {
        id: root_LogOver
        objectName: "ROOT_LogOver"
        Model {
            id: geo_LogOverObstacle_LOD0
            objectName: "GEO_LogOverObstacle_LOD0"
            source: "meshes/geo_LogOverObstacle_LOD0_mesh.mesh"
            materials: [
                mat_LogOverBark_Grey_material,
                mat_LogOverEndGrain_Grey_material
            ]
        }
        Model {
            id: geo_LogOverTile_LOD0
            objectName: "GEO_LogOverTile_LOD0"
            source: "meshes/geo_LogOverTile_LOD0_mesh.mesh"
            materials: [
                mat_LogOverBypass_Grey_material
            ]
        }
        Node {
            id: marker_ACTION
            objectName: "MARKER_ACTION"
            position: Qt.vector3d(0, 0, 0.75)
        }
        Node {
            id: marker_APEX
            objectName: "MARKER_APEX"
            position: Qt.vector3d(0, 0.54, 1.02)
        }
        Node {
            id: marker_DECISION
            objectName: "MARKER_DECISION"
            position: Qt.vector3d(0, 0, 0.375)
        }
        Node {
            id: marker_LAND
            objectName: "MARKER_LAND"
            position: Qt.vector3d(0, 0, 1.29)
        }
        Node {
            id: marker_PREPARE
            objectName: "MARKER_PREPARE"
        }
        Node {
            id: socket_IN
            objectName: "SOCKET_IN"
        }
        Node {
            id: socket_OUT
            objectName: "SOCKET_OUT"
            position: Qt.vector3d(0, 0, 2.04)
        }
    }

    // Animations:
}
