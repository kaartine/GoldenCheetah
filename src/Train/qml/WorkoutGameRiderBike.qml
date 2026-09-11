import QtQuick
import QtQuick3D

Node {
    id: root
    objectName: "riderNode"

    required property real distanceMeters
    required property real pedalAngle
    required property real airHeight
    required property real tailwhip
    required property bool rendererPrewarming
    required property real pump
    required property real standingBlend
    required property real pedalEffort
    required property real rearSuspensionCompression
    required property real frontSuspensionCompression
    required property bool walking
    required property string poseState
    required property real riderPitch
    required property real riderYaw
    required property real riderRoll

    readonly property real wheelRadius: 0.3775
    readonly property real wheelbase: 1.313
    readonly property real rearAxleZ: -0.455
    readonly property real frontAxleZ: 0.858
    readonly property real crankY: 0.3775
    readonly property real crankZ: 0
    readonly property real steerY: 1.085
    readonly property real steerZ: 0.465
    readonly property real thighLength: 0.45
    readonly property real shinLength: 0.45
    readonly property real upperArmLength: 0.32
    readonly property real forearmLength: 0.36
    readonly property real gripHalfSpan: 0.39
    readonly property real wheelAngle:
        -distanceMeters / (2 * Math.PI * wheelRadius) * 360
    property real actionHeight: poseState === "preload" ? -0.09
                                : poseState === "air" ? 0.06
                                : poseState === "land" ? -0.11
                                : poseState === "absorb" ? -0.06
                                : poseState === "coast" ? -0.02 : 0
    property real actionPitch: poseState === "preload" ? 7
                               : poseState === "air" ? -8
                               : poseState === "land" ? 10
                               : poseState === "absorb" ? 5
                               : poseState === "coast" ? -3
                               : poseState === "bypass" ? -2 : 0
    property real actionForward: poseState === "air" ? 0.05
                                 : poseState === "bypass" ? -0.04 : 0
    property real coastBlend: poseState === "coast" ? 1 : 0
    readonly property real rearTravel: 0.11 * Math.max(
        0, Math.min(1, rearSuspensionCompression))
    readonly property real frontTravel: 0.11 * Math.max(
        0, Math.min(1, frontSuspensionCompression))
    readonly property real frameHeave: -0.5 * (rearTravel + frontTravel)
    readonly property real framePitch: Math.atan2(
        rearTravel - frontTravel, wheelbase) * 180 / Math.PI

    function tailwhipPoint(point, amount) {
        const radians = tailwhip * amount * Math.PI / 180
        const z = point.z - frontAxleZ
        return Qt.vector3d(
            point.x * Math.cos(radians) + z * Math.sin(radians),
            point.y,
            frontAxleZ - point.x * Math.sin(radians)
                + z * Math.cos(radians))
    }

    Behavior on actionHeight { NumberAnimation { duration: 120 } }
    Behavior on actionPitch { NumberAnimation { duration: 120 } }
    Behavior on actionForward { NumberAnimation { duration: 120 } }
    Behavior on coastBlend { NumberAnimation { duration: 120 } }

    readonly property real crankRenderAngle:
        pedalAngle * (1 - coastBlend) + 90 * coastBlend
    readonly property real crankRadians: crankRenderAngle * Math.PI / 180
    readonly property real pedalStroke: Math.sin(crankRadians)
    readonly property real effortSway: pedalStroke * pedalEffort
                                  * (0.012 + 0.024 * standingBlend)
    readonly property real effortRoll: -pedalStroke * pedalEffort
                                  * (0.8 + 2.4 * standingBlend)
    readonly property real effortBob: (0.5 + 0.5 * Math.cos(
        2 * crankRadians)) * pedalEffort * (0.008 + 0.015 * standingBlend)
    readonly property real bodyX: effortSway
    readonly property real bodyY: 1.23 + pump + effortBob
                                  + 0.14 * standingBlend
                                  + actionHeight
                                  - (walking ? 0.10 : 0)
    readonly property real bodyZ: -0.03 + 0.12 * standingBlend
                                  + actionForward

    function riderFrustumScenePoints() {
        return [
            articulatedRider.mapPositionToScene(
                Qt.vector3d(0, 2 * wheelRadius, rearAxleZ)),
            articulatedRider.mapPositionToScene(
                Qt.vector3d(0, 0, rearAxleZ)),
            articulatedRider.mapPositionToScene(
                Qt.vector3d(0, wheelRadius, rearAxleZ + wheelRadius)),
            articulatedRider.mapPositionToScene(
                Qt.vector3d(0, wheelRadius, rearAxleZ - wheelRadius)),
            articulatedRider.mapPositionToScene(
                Qt.vector3d(0, 2 * wheelRadius, frontAxleZ)),
            articulatedRider.mapPositionToScene(
                Qt.vector3d(0, 0, frontAxleZ)),
            articulatedRider.mapPositionToScene(
                Qt.vector3d(0, wheelRadius, frontAxleZ + wheelRadius)),
            articulatedRider.mapPositionToScene(
                Qt.vector3d(0, wheelRadius, frontAxleZ - wheelRadius)),
            sprungBike.mapPositionToScene(
                Qt.vector3d(-0.34, steerY, steerZ)),
            sprungBike.mapPositionToScene(
                Qt.vector3d(0.34, steerY, steerZ)),
            body.mapPositionToScene(Qt.vector3d(-0.25, -0.30, -0.18)),
            body.mapPositionToScene(Qt.vector3d(0.25, -0.30, -0.18)),
            body.mapPositionToScene(Qt.vector3d(-0.25, 0.30, 0.08)),
            body.mapPositionToScene(Qt.vector3d(0.25, 0.30, 0.08)),
            body.mapPositionToScene(Qt.vector3d(-0.18, 0.62, -0.12)),
            body.mapPositionToScene(Qt.vector3d(0.18, 0.62, -0.12)),
            body.mapPositionToScene(Qt.vector3d(0, 0.70, 0.10))
        ]
    }

    function wheelFrustumScenePoints() {
        return riderFrustumScenePoints()
    }
    readonly property vector3d leftPedal: Qt.vector3d(
        -0.13,
        crankY + 0.16 * Math.cos(crankRadians),
        crankZ + 0.16 * Math.sin(crankRadians))
    readonly property vector3d rightPedal: Qt.vector3d(
        0.13,
        crankY - 0.16 * Math.cos(crankRadians),
        crankZ - 0.16 * Math.sin(crankRadians))

    function bodyAnchor(localPoint) {
        // Track both parts of the body transform before mapping the anchor.
        const bodyPosition = body.position
        const bodyRotation = body.eulerRotation
        const transformState = bodyPosition.x + bodyPosition.y + bodyPosition.z
                + bodyRotation.x + bodyRotation.y + bodyRotation.z
        if (!Number.isFinite(transformState))
            return Qt.vector3d(0, 0, 0)
        return body.mapPositionToNode(sprungBike, localPoint)
    }

    readonly property vector3d leftHip: bodyAnchor(
        Qt.vector3d(-0.12, -0.15, -0.09))
    readonly property vector3d rightHip: bodyAnchor(
        Qt.vector3d(0.12, -0.15, -0.09))
    readonly property vector3d leftKnee: twoBoneJoint(
        leftHip, leftPedal, thighLength, shinLength,
        Qt.vector3d(-0.08, 0, 1))
    readonly property vector3d rightKnee: twoBoneJoint(
        rightHip, rightPedal, thighLength, shinLength,
        Qt.vector3d(0.08, 0, 1))
    readonly property vector3d leftShoulder: bodyAnchor(
        Qt.vector3d(-0.19, 0.20, 0.08))
    readonly property vector3d rightShoulder: bodyAnchor(
        Qt.vector3d(0.19, 0.20, 0.08))
    readonly property vector3d leftHand: Qt.vector3d(
        -gripHalfSpan, steerY, steerZ)
    readonly property vector3d rightHand: Qt.vector3d(
        gripHalfSpan, steerY, steerZ)
    readonly property vector3d leftElbow: twoBoneJoint(
        leftShoulder, leftHand, upperArmLength, forearmLength,
        Qt.vector3d(-1, -0.25, -0.20))
    readonly property vector3d rightElbow: twoBoneJoint(
        rightShoulder, rightHand, upperArmLength, forearmLength,
        Qt.vector3d(1, -0.25, -0.20))

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

    function segmentDirection(from, to) {
        const length = segmentLength(from, to)
        if (length < 0.0001)
            return Qt.vector3d(0, 1, 0)
        return Qt.vector3d(
            (to.x - from.x) / length,
            (to.y - from.y) / length,
            (to.z - from.z) / length)
    }

    function offsetPoint(point, direction, amount) {
        return Qt.vector3d(
            point.x + direction.x * amount,
            point.y + direction.y * amount,
            point.z + direction.z * amount)
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

    function twoBoneJoint(start, end, firstLength, secondLength, bendHint) {
        const dx = end.x - start.x
        const dy = end.y - start.y
        const dz = end.z - start.z
        const actualDistance = Math.sqrt(dx * dx + dy * dy + dz * dz)
        const distance = Math.max(0.0001, Math.min(
            firstLength + secondLength - 0.0001, actualDistance))
        const ux = dx / Math.max(actualDistance, 0.0001)
        const uy = dy / Math.max(actualDistance, 0.0001)
        const uz = dz / Math.max(actualDistance, 0.0001)
        const along = (firstLength * firstLength
                       - secondLength * secondLength + distance * distance)
                      / (2 * distance)
        const height = Math.sqrt(Math.max(
            0, firstLength * firstLength - along * along))
        const projection = bendHint.x * ux + bendHint.y * uy + bendHint.z * uz
        let bx = bendHint.x - projection * ux
        let by = bendHint.y - projection * uy
        let bz = bendHint.z - projection * uz
        let bendLength = Math.sqrt(bx * bx + by * by + bz * bz)
        if (bendLength < 0.0001) {
            bx = 1
            by = 0
            bz = 0
            bendLength = 1
        }
        return Qt.vector3d(
            start.x + ux * along + bx / bendLength * height,
            start.y + uy * along + by / bendLength * height,
            start.z + uz * along + bz / bendLength * height)
    }

    PrincipledMaterial {
        id: bikeMaterial
        objectName: "riderBikeMaterial"
        baseColor: "#b88935"
        baseColorMap: riderPixelTexture
        metalness: 0.68
        roughness: 0.34
    }
    PrincipledMaterial {
        id: tireMaterial
        objectName: "riderTireMaterial"
        baseColor: "#252c2e"
        roughness: 0.92
    }
    PrincipledMaterial {
        id: componentMaterial
        objectName: "riderComponentMaterial"
        baseColor: "#293235"
        metalness: 0.28
        roughness: 0.52
    }
    PrincipledMaterial {
        id: jerseyMaterial
        objectName: "riderJerseyMaterial"
        baseColor: "#2f68b2"
        baseColorMap: riderPixelTexture
        roughness: 0.84
    }
    PrincipledMaterial {
        id: shortsMaterial
        objectName: "riderShortsMaterial"
        baseColor: "#303a3e"
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
        baseColor: "#d7dad8"
        baseColorMap: riderPixelTexture
        roughness: 0.54
    }
    PrincipledMaterial {
        id: riderDarkMaterial
        objectName: "riderDarkMaterial"
        baseColor: "#2b3437"
        baseColorMap: riderPixelTexture
        roughness: 0.82
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
        property real jointOverlap: Math.min(
            0.022, root.segmentLength(from, to) * 0.08)
        readonly property vector3d direction: root.segmentDirection(from, to)
        readonly property vector3d visualFrom: root.offsetPoint(
            from, direction, -jointOverlap)
        readonly property vector3d visualTo: root.offsetPoint(
            to, direction, jointOverlap)

        position: visualFrom
        eulerRotation.x: root.segmentPitch(visualFrom, visualTo)
        eulerRotation.z: root.segmentRoll(visualFrom, visualTo)
        Model {
            source: "assets/meshes/geo_Limb_LOD0_mesh.mesh"
            scale: Qt.vector3d(
                parent.thickness,
                root.segmentLength(parent.visualFrom, parent.visualTo),
                parent.thickness)
            materials: parent.segmentMaterial
            castsShadows: false
            receivesShadows: false
        }
    }

    Model {
        objectName: "riderGroundShadow"
        visible: root.rendererPrewarming || root.airHeight > 0.015
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

        Node {
            objectName: "rearWheelPivot"
            position: root.tailwhipPoint(
                Qt.vector3d(0, root.wheelRadius, root.rearAxleZ), 1)
            eulerRotation: Qt.vector3d(
                root.wheelAngle, root.tailwhip, 0)
            Model {
                source: "assets/meshes/geo_RearWheel_LOD0_mesh.mesh"
                position: Qt.vector3d(
                    0, -root.wheelRadius, -root.rearAxleZ)
                materials: tireMaterial
                castsShadows: false
                receivesShadows: false
            }
        }
        Node {
            objectName: "frontWheelPivot"
            position: Qt.vector3d(0, root.wheelRadius, root.frontAxleZ)
            eulerRotation.x: root.wheelAngle
            Model {
                source: "assets/meshes/geo_FrontWheel_LOD0_mesh.mesh"
                position: Qt.vector3d(
                    0, -root.wheelRadius, -root.frontAxleZ)
                materials: tireMaterial
                castsShadows: false
                receivesShadows: false
            }
        }

        Node {
            objectName: "rearSwingarmPivot"
            position: root.tailwhipPoint(
                Qt.vector3d(0, root.wheelRadius, root.rearAxleZ), 1)
            eulerRotation: Qt.vector3d(
                -root.rearTravel * 34, root.tailwhip, 0)
            Model {
                source: "assets/meshes/geo_Swingarm_LOD0_mesh.mesh"
                position: Qt.vector3d(
                    0, -root.wheelRadius, -root.rearAxleZ)
                materials: bikeMaterial
                castsShadows: false
                receivesShadows: false
            }
        }

        Node {
            objectName: "frontForkPivot"
            position: Qt.vector3d(0, root.wheelRadius, root.frontAxleZ)
            eulerRotation: Qt.vector3d(
                root.frontTravel * 18, root.tailwhip * 0.82, 0)
            Model {
                source: "assets/meshes/geo_Fork_LOD0_mesh.mesh"
                position: Qt.vector3d(
                    0, -root.wheelRadius, -root.frontAxleZ)
                materials: componentMaterial
                castsShadows: false
                receivesShadows: false
            }
        }

        Node {
            id: sprungBike
            objectName: "sprungBikeNode"
            position: root.tailwhipPoint(
                Qt.vector3d(0, root.frameHeave, 0), 1)
            eulerRotation: Qt.vector3d(
                root.framePitch, root.tailwhip, root.effortRoll * 0.22)

            Model {
                source: "assets/meshes/geo_MainFrame_LOD0_mesh.mesh"
                materials: bikeMaterial
                castsShadows: false
                receivesShadows: false
            }

            Model {
                source: "assets/meshes/geo_RearShock_LOD0_mesh.mesh"
                scale: Qt.vector3d(
                    1, Math.max(0.72, 1 - root.rearTravel * 1.8), 1)
                materials: componentMaterial
                castsShadows: false
                receivesShadows: false
            }

            Model {
                source: "assets/meshes/geo_BikeComponents_LOD0_mesh.mesh"
                materials: componentMaterial
                castsShadows: false
                receivesShadows: false
            }

            Node {
                objectName: "crankPivot"
                position: Qt.vector3d(0, root.crankY, root.crankZ)
                eulerRotation.x: root.crankRenderAngle
                Model {
                    source: "assets/meshes/geo_Crank_LOD0_mesh.mesh"
                    position: Qt.vector3d(0, -root.crankY, -root.crankZ)
                    materials: componentMaterial
                    castsShadows: false
                    receivesShadows: false
                }
            }

            Node {
                objectName: "leftPedalContact"
                position: root.leftPedal
            }

            Node {
                objectName: "rightPedalContact"
                position: root.rightPedal
            }

            Node {
                id: body
                objectName: "riderBodyNode"
                position: Qt.vector3d(root.bodyX, root.bodyY, root.bodyZ)
                eulerRotation: Qt.vector3d(
                    3 * root.standingBlend + root.actionPitch * 0.35
                        - (root.walking ? 5 : 0),
                    0,
                    root.effortRoll)
                Model {
                    source: "assets/meshes/geo_Torso_LOD0_mesh.mesh"
                    position: Qt.vector3d(0, -0.15, -0.09)
                    materials: jerseyMaterial
                    castsShadows: false
                    receivesShadows: false
                }
                Model {
                    source: "assets/meshes/geo_JerseyAccent_LOD0_mesh.mesh"
                    position: Qt.vector3d(0, -0.15, -0.09)
                    materials: helmetMaterial
                    castsShadows: false
                    receivesShadows: false
                }
                Model {
                    source: "assets/meshes/geo_Torso_LOD0_mesh.mesh"
                    position: Qt.vector3d(0, -0.16, -0.11)
                    scale: Qt.vector3d(0.72, 0.36, 0.68)
                    materials: shortsMaterial
                    castsShadows: false
                    receivesShadows: false
                }
                Node {
                    position: Qt.vector3d(0, 0.42, 0.23)
                    Model {
                        source: "assets/meshes/geo_Head_LOD0_mesh.mesh"
                        materials: skinMaterial
                        castsShadows: false
                        receivesShadows: false
                    }
                    Model {
                        source: "assets/meshes/geo_HairBeard_LOD0_mesh.mesh"
                        materials: riderDarkMaterial
                        castsShadows: false
                        receivesShadows: false
                    }
                    Model {
                        source: "assets/meshes/geo_Eyewear_LOD0_mesh.mesh"
                        materials: componentMaterial
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
                    Model {
                        source: "assets/meshes/geo_HelmetAccent_LOD0_mesh.mesh"
                        y: 0.045
                        materials: riderDarkMaterial
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
                objectName: "rightUpperLeg"
                from: root.rightHip
                to: root.rightKnee
                segmentMaterial: shortsMaterial
            }
            LimbSegment {
                objectName: "rightLowerLeg"
                from: root.rightKnee
                to: root.rightPedal
                thickness: 0.62
                segmentMaterial: skinMaterial
            }

            LimbSegment {
                objectName: "leftUpperArm"
                from: root.leftShoulder
                to: root.leftElbow
                thickness: 0.58
            }
            LimbSegment {
                objectName: "leftForearm"
                from: root.leftElbow
                to: root.leftHand
                thickness: 0.52
                segmentMaterial: skinMaterial
            }
            LimbSegment {
                objectName: "rightUpperArm"
                from: root.rightShoulder
                to: root.rightElbow
                thickness: 0.58
            }
            LimbSegment {
                objectName: "rightForearm"
                from: root.rightElbow
                to: root.rightHand
                thickness: 0.52
                segmentMaterial: skinMaterial
            }
            LimbSegment {
                objectName: "leftShoe"
                from: Qt.vector3d(root.leftPedal.x, root.leftPedal.y,
                                  root.leftPedal.z - 0.065)
                to: Qt.vector3d(root.leftPedal.x, root.leftPedal.y,
                                root.leftPedal.z + 0.075)
                thickness: 0.58
                segmentMaterial: riderDarkMaterial
            }
            LimbSegment {
                objectName: "rightShoe"
                from: Qt.vector3d(root.rightPedal.x, root.rightPedal.y,
                                  root.rightPedal.z - 0.065)
                to: Qt.vector3d(root.rightPedal.x, root.rightPedal.y,
                                root.rightPedal.z + 0.075)
                thickness: 0.58
                segmentMaterial: riderDarkMaterial
            }
            LimbSegment {
                objectName: "leftGlove"
                from: Qt.vector3d(-0.45, root.steerY, root.steerZ)
                to: Qt.vector3d(-0.34, root.steerY, root.steerZ)
                thickness: 0.48
                segmentMaterial: riderDarkMaterial
            }
            LimbSegment {
                objectName: "rightGlove"
                from: Qt.vector3d(0.34, root.steerY, root.steerZ)
                to: Qt.vector3d(0.45, root.steerY, root.steerZ)
                thickness: 0.48
                segmentMaterial: riderDarkMaterial
            }
        }
    }
}
