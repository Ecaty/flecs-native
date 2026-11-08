#include "FlecsComponents.hpp"
#include <cmath>

namespace flecs_unity {

void register_core(flecs::world& world) {
    world.component<Clock>("Clock")
        .member(&Clock::time, "time")
        .member(&Clock::delta, "delta")
        .member(&Clock::frame, "frame");
    world.set<Clock>({});
    world.component<Position>("Position")
        .member(&Position::x, "x")
        .member(&Position::y, "y")
        .member(&Position::z, "z");
    world.component<Rotation>("Rotation")
        .member(&Rotation::x, "x")
        .member(&Rotation::y, "y")
        .member(&Rotation::z, "z");
    world.component<Scale>("Scale")
        .member(&Scale::x, "x")
        .member(&Scale::y, "y")
        .member(&Scale::z, "z");
    world.component<Color>("Color")
        .member(&Color::r, "r")
        .member(&Color::g, "g")
        .member(&Color::b, "b")
        .member(&Color::a, "a");
    world.component<Presented>("Presented")
        .member(&Presented::position, "position")
        .member(&Presented::rotation, "rotation")
        .member(&Presented::scale, "scale")
        .member(&Presented::color, "color")
        .member(&Presented::frame, "frame")
        .member(&Presented::primitive, "primitive")
        .member(&Presented::has_label, "has_label");
    world.component<Primitive>("Primitive")
        .member(&Primitive::kind, "kind");
    world.component<Label>("Label")
        .member(flecs::String, "text");

}

} // namespace flecs_unity
