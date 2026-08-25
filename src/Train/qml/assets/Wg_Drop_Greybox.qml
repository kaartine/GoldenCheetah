import QtQuick
import QtQuick3D

Node {
    id: node

    // Resources
    PrincipledMaterial {
        id: mat_DropFace_Grey_material
        objectName: "MAT_DropFace_Grey"
        baseColor: "#ff40382e"
        roughness: 1
        cullMode: PrincipledMaterial.NoCulling
        alphaMode: PrincipledMaterial.Opaque
    }
    PrincipledMaterial {
        id: mat_DropEdge_Grey_material
        objectName: "MAT_DropEdge_Grey"
        baseColor: "#ff6e634f"
        roughness: 1
        cullMode: PrincipledMaterial.NoCulling
        alphaMode: PrincipledMaterial.Opaque
    }

    // Nodes:
    Node {
        id: root_Drop
        objectName: "ROOT_Drop"
        Model {
            id: geo_DropFace_LOD0
            objectName: "GEO_DropFace_LOD0"
            source: "meshes/geo_DropFace_LOD0_mesh.mesh"
            materials: [
                mat_DropFace_Grey_material,
                mat_DropEdge_Grey_material
            ]
        }
        Node {
            id: marker_ACTION
            objectName: "MARKER_ACTION"
            position: Qt.vector3d(0, 0, 9.5)
        }
        Node {
            id: marker_AIR
            objectName: "MARKER_AIR"
            position: Qt.vector3d(0, -0.2, 10.65)
        }
        Node {
            id: marker_DECISION
            objectName: "MARKER_DECISION"
            position: Qt.vector3d(0, 0, 6)
        }
        Node {
            id: marker_LAND
            objectName: "MARKER_LAND"
            position: Qt.vector3d(0, -0.7, 12.5)
        }
        Node {
            id: marker_LIP
            objectName: "MARKER_LIP"
            position: Qt.vector3d(0, 0, 10)
        }
        Node {
            id: marker_PREPARE
            objectName: "MARKER_PREPARE"
        }
        Node {
            id: marker_RECOVERY
            objectName: "MARKER_RECOVERY"
            position: Qt.vector3d(0, -0.7, 15)
        }
        Node {
            id: socket_IN
            objectName: "SOCKET_IN"
        }
        Node {
            id: socket_OUT
            objectName: "SOCKET_OUT"
            position: Qt.vector3d(0, 0, 22)
        }
    }

    // Animations:
}
