import QtQuick
import QtQuick3D

Node {
    id: node

    // Resources
    PrincipledMaterial {
        id: mat_GapJumpCutEarth_material
        objectName: "MAT_GapJumpCutEarth"
        baseColor: "#ff6b4b31"
        roughness: 1
        cullMode: PrincipledMaterial.NoCulling
        alphaMode: PrincipledMaterial.Opaque
    }
    PrincipledMaterial {
        id: mat_GapJumpForestFloor_material
        objectName: "MAT_GapJumpForestFloor"
        baseColor: "#ff26553d"
        roughness: 1
        cullMode: PrincipledMaterial.NoCulling
        alphaMode: PrincipledMaterial.Opaque
    }
    PrincipledMaterial {
        id: mat_GapJumpPackedDirt_material
        objectName: "MAT_GapJumpPackedDirt"
        baseColor: "#ff915c34"
        roughness: 1
        cullMode: PrincipledMaterial.NoCulling
        alphaMode: PrincipledMaterial.Opaque
    }

    // Nodes:
    Node {
        id: root_GapJumpThreeLine
        objectName: "ROOT_GapJumpThreeLine"
        Model {
            id: geo_GapJumpAccents_LOD0
            objectName: "GEO_GapJumpAccents_LOD0"
            source: "meshes/geo_GapJumpAccents_LOD0_mesh.mesh"
            materials: [
                mat_GapJumpCutEarth_material
            ]
        }
        Model {
            id: geo_GapJumpGround_LOD0
            objectName: "GEO_GapJumpGround_LOD0"
            source: "meshes/geo_GapJumpGround_LOD0_mesh.mesh"
            materials: [
                mat_GapJumpForestFloor_material
            ]
        }
        Model {
            id: geo_GapJumpTread_LOD0
            objectName: "GEO_GapJumpTread_LOD0"
            source: "meshes/geo_GapJumpTread_LOD0_mesh.mesh"
            materials: [
                mat_GapJumpPackedDirt_material,
                mat_GapJumpCutEarth_material
            ]
        }
        Node {
            id: marker_DECISION
            objectName: "MARKER_DECISION"
            position: Qt.vector3d(0, 0, 9)
        }
        Node {
            id: marker_LONG_APEX
            objectName: "MARKER_LONG_APEX"
            position: Qt.vector3d(2.3, 1.806, 14.444)
        }
        Node {
            id: marker_LONG_LAND
            objectName: "MARKER_LONG_LAND"
            position: Qt.vector3d(2.3, 0.46, 16.7)
        }
        Node {
            id: marker_LONG_LIP
            objectName: "MARKER_LONG_LIP"
            position: Qt.vector3d(2.3, 0.78, 12)
        }
        Node {
            id: marker_MEDIUM_APEX
            objectName: "MARKER_MEDIUM_APEX"
            position: Qt.vector3d(0, 1.526, 13.664)
        }
        Node {
            id: marker_MEDIUM_LAND
            objectName: "MARKER_MEDIUM_LAND"
            position: Qt.vector3d(0, 0.4, 15.2)
        }
        Node {
            id: marker_MEDIUM_LIP
            objectName: "MARKER_MEDIUM_LIP"
            position: Qt.vector3d(0, 0.62, 12)
        }
        Node {
            id: marker_MERGE_START
            objectName: "MARKER_MERGE_START"
            position: Qt.vector3d(0, 0, 22.7)
        }
        Node {
            id: marker_RECOVERY
            objectName: "MARKER_RECOVERY"
            position: Qt.vector3d(0, 0, 36.5)
        }
        Node {
            id: marker_SHORT_APEX
            objectName: "MARKER_SHORT_APEX"
            position: Qt.vector3d(-2.3, 1.274, 12.936)
        }
        Node {
            id: marker_SHORT_LAND
            objectName: "MARKER_SHORT_LAND"
            position: Qt.vector3d(-2.3, 0.34, 13.8)
        }
        Node {
            id: marker_SHORT_LIP
            objectName: "MARKER_SHORT_LIP"
            position: Qt.vector3d(-2.3, 0.48, 12)
        }
        Node {
            id: socket_IN
            objectName: "SOCKET_IN"
        }
        Node {
            id: socket_OUT
            objectName: "SOCKET_OUT"
            position: Qt.vector3d(0, 0, 40.7)
        }
    }

    // Animations:
}
