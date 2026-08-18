isEmpty(BOX2D_ROOT): error("BOX2D_ROOT must name the vendored Box2D directory")

INCLUDEPATH += $$BOX2D_ROOT/include
DEPENDPATH += $$BOX2D_ROOT/include $$BOX2D_ROOT/src

!msvc {
    QMAKE_CFLAGS += -std=gnu17 -ffp-contract=off -Wno-unused-value
}

SOURCES += \
    $$BOX2D_ROOT/src/aabb.c \
    $$BOX2D_ROOT/src/arena_allocator.c \
    $$BOX2D_ROOT/src/array.c \
    $$BOX2D_ROOT/src/bitset.c \
    $$BOX2D_ROOT/src/body.c \
    $$BOX2D_ROOT/src/broad_phase.c \
    $$BOX2D_ROOT/src/constraint_graph.c \
    $$BOX2D_ROOT/src/contact.c \
    $$BOX2D_ROOT/src/contact_solver.c \
    $$BOX2D_ROOT/src/core.c \
    $$BOX2D_ROOT/src/distance.c \
    $$BOX2D_ROOT/src/distance_joint.c \
    $$BOX2D_ROOT/src/dynamic_tree.c \
    $$BOX2D_ROOT/src/geometry.c \
    $$BOX2D_ROOT/src/hull.c \
    $$BOX2D_ROOT/src/id_pool.c \
    $$BOX2D_ROOT/src/island.c \
    $$BOX2D_ROOT/src/joint.c \
    $$BOX2D_ROOT/src/manifold.c \
    $$BOX2D_ROOT/src/math_functions.c \
    $$BOX2D_ROOT/src/motor_joint.c \
    $$BOX2D_ROOT/src/mouse_joint.c \
    $$BOX2D_ROOT/src/mover.c \
    $$BOX2D_ROOT/src/prismatic_joint.c \
    $$BOX2D_ROOT/src/revolute_joint.c \
    $$BOX2D_ROOT/src/sensor.c \
    $$BOX2D_ROOT/src/shape.c \
    $$BOX2D_ROOT/src/solver.c \
    $$BOX2D_ROOT/src/solver_set.c \
    $$BOX2D_ROOT/src/table.c \
    $$BOX2D_ROOT/src/timer.c \
    $$BOX2D_ROOT/src/types.c \
    $$BOX2D_ROOT/src/weld_joint.c \
    $$BOX2D_ROOT/src/wheel_joint.c \
    $$BOX2D_ROOT/src/world.c

HEADERS += \
    $$files($$BOX2D_ROOT/include/box2d/*.h) \
    $$files($$BOX2D_ROOT/src/*.h)
