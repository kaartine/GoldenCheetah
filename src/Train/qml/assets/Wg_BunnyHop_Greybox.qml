import QtQuick
import QtQuick3D

Node {
    id: node

    // Resources
    PrincipledMaterial {
        id: mat_BunnyHopBar_Grey_material
        objectName: "MAT_BunnyHopBar_Grey"
        baseColor: "#ffe0ab40"
        roughness: 1
        cullMode: PrincipledMaterial.NoCulling
        alphaMode: PrincipledMaterial.Opaque
    }
    PrincipledMaterial {
        id: mat_BunnyHopSupport_Grey_material
        objectName: "MAT_BunnyHopSupport_Grey"
        baseColor: "#ff3d1f0b"
        roughness: 1
        cullMode: PrincipledMaterial.NoCulling
        alphaMode: PrincipledMaterial.Opaque
    }

    // Nodes:
    Node {
        id: root_BunnyHop
        objectName: "ROOT_BunnyHop"
        Model {
            id: geo_BunnyHopHurdle_LOD0
            objectName: "GEO_BunnyHopHurdle_LOD0"
            source: "meshes/geo_BunnyHopHurdle_LOD0_mesh.mesh"
            materials: [
                mat_BunnyHopBar_Grey_material,
                mat_BunnyHopSupport_Grey_material
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
            position: Qt.vector3d(0, 0.2, 1.79)
        }
        Node {
            id: marker_DECISION
            objectName: "MARKER_DECISION"
            position: Qt.vector3d(0, 0, 0.375)
        }
        Node {
            id: marker_LAND
            objectName: "MARKER_LAND"
            position: Qt.vector3d(0, 0, 2.83)
        }
        Node {
            id: marker_PRELOAD
            objectName: "MARKER_PRELOAD"
            position: Qt.vector3d(0, 0, 0.75)
        }
        Node {
            id: marker_PREPARE
            objectName: "MARKER_PREPARE"
        }
        Node {
            id: marker_TAKEOFF
            objectName: "MARKER_TAKEOFF"
            position: Qt.vector3d(0, 0, 1.2)
        }
        Node {
            id: socket_IN
            objectName: "SOCKET_IN"
        }
        Node {
            id: socket_OUT
            objectName: "SOCKET_OUT"
            position: Qt.vector3d(0, 0, 3.58)
        }
    }

    // Animations:
}
