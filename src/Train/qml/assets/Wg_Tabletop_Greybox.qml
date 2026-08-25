import QtQuick
import QtQuick3D

Node {
    id: node

    // Resources
    PrincipledMaterial {
        id: mat_TabletopTrail_Grey_material
        objectName: "MAT_TabletopTrail_Grey"
        baseColor: "#ff636159"
        roughness: 1
        cullMode: PrincipledMaterial.NoCulling
        alphaMode: PrincipledMaterial.Opaque
    }
    PrincipledMaterial {
        id: mat_TabletopTerrain_Grey_material
        objectName: "MAT_TabletopTerrain_Grey"
        baseColor: "#ff4a524a"
        roughness: 1
        cullMode: PrincipledMaterial.NoCulling
        alphaMode: PrincipledMaterial.Opaque
    }
    PrincipledMaterial {
        id: mat_TabletopSkirt_Grey_material
        objectName: "MAT_TabletopSkirt_Grey"
        baseColor: "#ff2e302e"
        roughness: 1
        cullMode: PrincipledMaterial.NoCulling
        alphaMode: PrincipledMaterial.Opaque
    }

    // Nodes:
    Node {
        id: root_Tabletop
        objectName: "ROOT_Tabletop"
        Model {
            id: geo_Tabletop_LOD0
            objectName: "GEO_Tabletop_LOD0"
            source: "meshes/geo_Tabletop_LOD0_mesh.mesh"
            materials: [
                mat_TabletopTrail_Grey_material,
                mat_TabletopTerrain_Grey_material,
                mat_TabletopSkirt_Grey_material
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
            position: Qt.vector3d(0, 0.446, 3.17)
        }
        Node {
            id: marker_DECISION
            objectName: "MARKER_DECISION"
            position: Qt.vector3d(0, 0, 0.375)
        }
        Node {
            id: marker_LAND
            objectName: "MARKER_LAND"
            position: Qt.vector3d(0, 0, 5.59)
        }
        Node {
            id: marker_LIP
            objectName: "MARKER_LIP"
            position: Qt.vector3d(0, 0.446, 2.62)
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
            position: Qt.vector3d(0, 0, 6.34)
        }
    }

    // Animations:
}
