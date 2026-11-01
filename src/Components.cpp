#include "FlecsComponents.hpp"
#include <cmath>

namespace flecs_unity {

void register_core(flecs::world& world) {
    world.component<Position>("Position")
        .member("x", &Position::x)
        .member("y", &Position::y)
        .member("z", &Position::z);
    world.component<Rotation>("Rotation")
        .member("x", &Rotation::x)
        .member("y", &Rotation::y)
        .member("z", &Rotation::z);
    world.component<Scale>("Scale")
        .member("x", &Scale::x)
        .member("y", &Scale::y)
        .member("z", &Scale::z);
    world.component<Color>("Color")
        .member("r", &Color::r)
        .member("g", &Color::g)
        .member("b", &Color::b)
        .member("a", &Color::a);
    world.component<Primitive>("Primitive")
        .member("kind", &Primitive::kind);
    world.component<Label>("Label")
        .member(flecs::String, "text");

}

} // namespace flecs_unity
