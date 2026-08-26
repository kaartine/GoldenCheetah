import QtQuick
import QtQuick3D

Node {
    id: root
    objectName: "riderNode"

    required property real distanceMeters
    required property real pedalAngle
    required property real airHeight
    required property real pump
    required property real standingBlend
    required property bool walking
    required property real riderPitch
    required property real riderYaw
    required property real riderRoll

    readonly property real wheelRadius: 0.3683
    readonly property real wheelAngle:
        -distanceMeters / (2 * Math.PI * wheelRadius) * 360
    readonly property real bodyY: 1.23 + pump + 0.14 * standingBlend
                                  - (walking ? 0.10 : 0)
    readonly property real bodyZ: -0.03 + 0.12 * standingBlend
    readonly property real pelvisY: bodyY - 0.15
    readonly property real pelvisZ: bodyZ - 0.09
    readonly property real crankRadians: pedalAngle * Math.PI / 180
    readonly property vector3d leftPedal: Qt.vector3d(
        -0.13,
        0.58 + 0.16 * Math.cos(crankRadians),
        -0.08 + 0.16 * Math.sin(crankRadians))
    readonly property vector3d rightPedal: Qt.vector3d(
        0.13,
        0.58 - 0.16 * Math.cos(crankRadians),
        -0.08 - 0.16 * Math.sin(crankRadians))
    readonly property vector3d leftHip: Qt.vector3d(-0.12, pelvisY, pelvisZ)
    readonly property vector3d rightHip: Qt.vector3d(0.12, pelvisY, pelvisZ)
    readonly property vector3d leftKnee: Qt.vector3d(
        -0.13, (leftHip.y + leftPedal.y) * 0.5 + 0.12,
        (leftHip.z + leftPedal.z) * 0.5 + 0.10)
    readonly property vector3d rightKnee: Qt.vector3d(
        0.13, (rightHip.y + rightPedal.y) * 0.5 + 0.12,
        (rightHip.z + rightPedal.z) * 0.5 + 0.10)

    Texture {
        id: riderPixelTexture
        objectName: "riderPixelTexture"
        source: "qrc:/images/workout-game-surface-rider.png"
        tilingModeHorizontal: Texture.Repeat
        tilingModeVertical: Texture.Repeat
        minFilter: Texture.Linear
        magFilter: Texture.Nearest
        generateMipmaps: true
    }

    function segmentLength(from, to) {
        const dx = to.x - from.x
        const dy = to.y - from.y
        const dz = to.z - from.z
        return Math.sqrt(dx * dx + dy * dy + dz * dz)
    }

    function segmentPitch(from, to) {
        return Math.atan2(to.z - from.z, to.y - from.y) * 180 / Math.PI
    }

    function segmentRoll(from, to) {
        const dy = to.y - from.y
        const dz = to.z - from.z
        return -Math.atan2(to.x - from.x, Math.sqrt(dy * dy + dz * dz))
                * 180 / Math.PI
    }

    PrincipledMaterial {
        id: bikeMaterial
        objectName: "riderBikeMaterial"
        baseColor: "#e6a51b"
        baseColorMap: riderPixelTexture
        metalness: 0.15
        roughness: 0.62
    }
    PrincipledMaterial {
        id: tireMaterial
        baseColor: "#151a1b"
        roughness: 0.92
    }
    PrincipledMaterial {
        id: jerseyMaterial
        objectName: "riderJerseyMaterial"
        baseColor: "#d7322d"
        baseColorMap: riderPixelTexture
        roughness: 0.84
    }
    PrincipledMaterial {
        id: shortsMaterial
        objectName: "riderShortsMaterial"
        baseColor: "#20272b"
        baseColorMap: riderPixelTexture
        roughness: 0.90
    }
    PrincipledMaterial {
        id: skinMaterial
        baseColor: "#dc9a6a"
        roughness: 0.90
    }
    PrincipledMaterial {
        id: helmetMaterial
        objectName: "riderHelmetMaterial"
        baseColor: "#ffc229"
        baseColorMap: riderPixelTexture
        roughness: 0.72
    }
    PrincipledMaterial {
        id: shadowMaterial
        baseColor: "#171c1b"
        roughness: 1
        alphaMode: PrincipledMaterial.Blend
    }

    component LimbSegment: Node {
        required property vector3d from
        required property vector3d to
        property real thickness: 0.72
        property var segmentMaterial: jerseyMaterial

        position: from
        eulerRotation.x: root.segmentPitch(from, to)
        eulerRotation.z: root.segmentRoll(from, to)
        Model {
            source: "assets/meshes/geo_Limb_LOD0_mesh.mesh"
            scale: Qt.vector3d(
                parent.thickness,
                root.segmentLength(parent.from, parent.to),
                parent.thickness)
            materials: parent.segmentMaterial
            castsShadows: false
            receivesShadows: false
        }
    }

    Model {
        objectName: "riderGroundShadow"
        source: "assets/meshes/geo_Shadow_LOD0_mesh.mesh"
        y: -airHeight
        scale: Qt.vector3d(
            Math.max(0.48, 1.0 - airHeight * 0.40),
            1,
            Math.max(0.48, 1.0 - airHeight * 0.40))
        opacity: Math.max(0.20, 0.52 - airHeight * 0.24)
        materials: shadowMaterial
        castsShadows: false
        receivesShadows: false
    }

    Node {
        id: articulatedRider
        objectName: "articulatedRiderNode"
        eulerRotation: Qt.vector3d(riderPitch, riderYaw, riderRoll)

        Model {
            source: "assets/meshes/geo_Frame_LOD0_mesh.mesh"
            materials: bikeMaterial
            castsShadows: false
            receivesShadows: false
        }

        Node {
            objectName: "rearWheelPivot"
            position: Qt.vector3d(0, 0.3683, -0.58)
            eulerRotation.x: root.wheelAngle
            Model {
                source: "assets/meshes/geo_RearWheel_LOD0_mesh.mesh"
                position: Qt.vector3d(0, -0.3683, 0.58)
                materials: tireMaterial
                castsShadows: false
                receivesShadows: false
            }
        }
        Node {
            objectName: "frontWheelPivot"
            position: Qt.vector3d(0, 0.3683, 0.58)
            eulerRotation.x: root.wheelAngle
            Model {
                source: "assets/meshes/geo_FrontWheel_LOD0_mesh.mesh"
                position: Qt.vector3d(0, -0.3683, -0.58)
                materials: tireMaterial
                castsShadows: false
                receivesShadows: false
            }
        }
        Node {
            objectName: "crankPivot"
            position: Qt.vector3d(0, 0.58, -0.08)
            eulerRotation.x: root.pedalAngle
            Model {
                source: "assets/meshes/geo_Crank_LOD0_mesh.mesh"
                position: Qt.vector3d(0, -0.58, 0.08)
                materials: bikeMaterial
                castsShadows: false
                receivesShadows: false
            }
        }

        Node {
            id: body
            objectName: "riderBodyNode"
            position: Qt.vector3d(0, root.bodyY, root.bodyZ)
            eulerRotation.x: 13 + 8 * root.standingBlend
                               - (root.walking ? 5 : 0)
            Model {
                source: "assets/meshes/geo_Torso_LOD0_mesh.mesh"
                position: Qt.vector3d(0, -0.15, -0.09)
                materials: jerseyMaterial
                castsShadows: false
                receivesShadows: false
            }
            Model {
                source: "assets/meshes/geo_Torso_LOD0_mesh.mesh"
                position: Qt.vector3d(0, -0.11, -0.19)
                scale: Qt.vector3d(0.72, 0.70, 0.60)
                materials: shortsMaterial
                castsShadows: false
                receivesShadows: false
            }
            Node {
                position: Qt.vector3d(0, 0.42, -0.03)
                Model {
                    source: "assets/meshes/geo_Head_LOD0_mesh.mesh"
                    materials: skinMaterial
                    castsShadows: false
                    receivesShadows: false
                }
                Model {
                    source: "assets/meshes/geo_Helmet_LOD0_mesh.mesh"
                    y: 0.045
                    materials: helmetMaterial
                    castsShadows: false
                    receivesShadows: false
                }
            }
        }

        LimbSegment {
            objectName: "leftUpperLeg"
            from: root.leftHip
            to: root.leftKnee
            segmentMaterial: shortsMaterial
        }
        LimbSegment {
            objectName: "leftLowerLeg"
            from: root.leftKnee
            to: root.leftPedal
            thickness: 0.62
            segmentMaterial: skinMaterial
        }
        LimbSegment {
            from: root.rightHip
            to: root.rightKnee
            segmentMaterial: shortsMaterial
        }
        LimbSegment {
            from: root.rightKnee
            to: root.rightPedal
            thickness: 0.62
            segmentMaterial: skinMaterial
        }

        LimbSegment {
            from: Qt.vector3d(-0.19, root.bodyY + 0.24, root.bodyZ - 0.04)
            to: Qt.vector3d(-0.25, root.bodyY + 0.07, root.bodyZ + 0.16)
            thickness: 0.58
        }
        LimbSegment {
            from: Qt.vector3d(-0.25, root.bodyY + 0.07, root.bodyZ + 0.16)
            to: Qt.vector3d(-0.28, 0.96, 0.43)
            thickness: 0.52
            segmentMaterial: skinMaterial
        }
        LimbSegment {
            from: Qt.vector3d(0.19, root.bodyY + 0.24, root.bodyZ - 0.04)
            to: Qt.vector3d(0.25, root.bodyY + 0.07, root.bodyZ + 0.16)
            thickness: 0.58
        }
        LimbSegment {
            from: Qt.vector3d(0.25, root.bodyY + 0.07, root.bodyZ + 0.16)
            to: Qt.vector3d(0.28, 0.96, 0.43)
            thickness: 0.52
            segmentMaterial: skinMaterial
        }
    }
}
