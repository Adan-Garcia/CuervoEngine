// vecN values (LANGUAGE.md 4.2, 9.10): construction, arithmetic, swizzles,
// component assignment, methods.

#include <doctest.h>

#include "run_helpers.hpp"

using test::panics;
using test::prints;
using test::rejects;

TEST_CASE("vec: construction and printing") {
    prints("print(vec2(1.0, 2.0))", "vec2(1.0, 2.0)\n");
    prints("print(vec2(1, 2))", "vec2(1.0, 2.0)\n"); // int literals adapt to float
    prints("print(vec3(0.5))", "vec3(0.5, 0.5, 0.5)\n");
    prints("var p = vec2(1.0, 2.0)\nprint(vec4(p, 3.0, 4.0))", "vec4(1.0, 2.0, 3.0, 4.0)\n");
    prints("var v: vec2\nprint(v)", "vec2(0.0, 0.0)\n");
    prints("var v: vec6\nprint(v[5])", "0.0\n");
    prints("print(vec1(7.0))", "vec1(7.0)\n");
}

TEST_CASE("vec: arithmetic") {
    prints("var a = vec2(1.0, 2.0)\nvar b = vec2(3.0, 5.0)\nprint(a + b)\nprint(b - a)\nprint(a * b)\nprint(b / a)",
           "vec2(4.0, 7.0)\nvec2(2.0, 3.0)\nvec2(3.0, 10.0)\nvec2(3.0, 2.5)\n");
    prints("var a = vec2(1.0, 2.0)\nprint(a * 2)\nprint(2.0 * a)\nprint(a / 2.0)\nprint(-a)",
           "vec2(2.0, 4.0)\nvec2(2.0, 4.0)\nvec2(0.5, 1.0)\nvec2(-1.0, -2.0)\n");
    prints("var a = vec3(1.0, 2.0, 3.0)\nvar b = a\nb += vec3(1.0)\nprint(a)\nprint(b)\nprint(a == b)\nprint(a != b)",
           "vec3(1.0, 2.0, 3.0)\nvec3(2.0, 3.0, 4.0)\nfalse\ntrue\n");
    prints("var p = vec2(0.0, 0.0)\nvar v = vec2(10.0, -5.0)\nvar dt = 0.5\np += v * dt\nprint(p)", "vec2(5.0, -2.5)\n");
}

TEST_CASE("vec: swizzles and component assignment") {
    prints("var v = vec3(1.0, 2.0, 3.0)\nprint(v.x)\nprint(v.zy)\nprint(v.xxyy)", "1.0\nvec2(3.0, 2.0)\nvec4(1.0, 1.0, 2.0, 2.0)\n");
    prints("var v = vec2(1.0, 2.0)\nv.x = 5.0\nv.y += 1.0\nprint(v)", "vec2(5.0, 3.0)\n");
    prints("var v = vec4(0.0)\nv.yz = vec2(1.0, 2.0)\nprint(v)", "vec4(0.0, 1.0, 2.0, 0.0)\n");
    prints("class A { var pos = vec2(1.0, 1.0) }\nvar a = A()\na.pos.y = 9.0\na.pos.x *= 3.0\nprint(a.pos)",
           "vec2(3.0, 9.0)\n");
    prints("var l = [vec2(1.0, 1.0)]\nl[0].x = 4.0\nprint(l[0])", "vec2(4.0, 1.0)\n");
    rejects("fn f(v: vec2) { v.x = 1.0 }",
            "1:17: cannot assign to a component of an immutable vector (a parameter, constant or temporary); copy it "
            "into a 'var'\n");
    rejects("var v = vec2(1.0, 2.0)\nprint(v.z)",
            "2:9: 'vec2' has no component 'z' (components are x, y, z, w; methods need a call)\n");
    rejects("var v = vec2(1.0, 2.0)\nv.xx = vec2(1.0, 1.0)", "2:3: 'xx' names a component twice\n");
}

TEST_CASE("vec: methods and math") {
    prints("var v = vec2(3.0, 4.0)\nprint(v.length())\nprint(v.length_squared())\nprint(v.normalized())",
           "5.0\n25.0\nvec2(0.6, 0.8)\n");
    prints("print(vec2(0.0, 0.0).normalized())", "vec2(0.0, 0.0)\n");
    prints("var a = vec2(1.0, 0.0)\nvar b = vec2(0.0, 1.0)\nprint(a.dot(b))\nprint(a.distance(b) * a.distance(b))\n"
           "print(a.lerp(b, 0.5))",
           "0.0\n1.9999999\nvec2(0.5, 0.5)\n");
    prints("print(vec2(-1.0, 5.0).clamp(vec2(0.0, 0.0), vec2(2.0, 2.0)))", "vec2(0.0, 2.0)\n");
    prints("print(vec2(-1.0, 5.0).abs())\nprint(vec2(1.0, 5.0).min(vec2(2.0, 3.0)))", "vec2(1.0, 5.0)\nvec2(1.0, 3.0)\n");
    prints("print(floor(2.5))\nprint(ceil(2.5))\nprint(round(2.5))", "2.0\n3.0\n3.0\n");
    prints("var r = vec2(1.0, 0.0).rotated(1.5707964)\nprint(abs(r.x) < 0.0001 && abs(r.y - 1.0) < 0.0001)", "true\n");
    prints("print(abs(vec2(0.0, 2.0).angle() - 1.5707964) < 0.0001)", "true\n");
    prints("var d = from_angle(0.0, 3.0)\nprint(d)", "vec2(3.0, 0.0)\n");
    rejects("print(vec3(1.0, 2.0, 3.0).rotated(1.0))", "1:27: 'vec3' has no method 'rotated'\n");
}

TEST_CASE("vec: extended methods and geometric math") {
    prints("print(vec2(1.0, 0.0).cross(vec2(0.0, 1.0)))", "1.0\n");
    prints("print(vec3(1.0, 0.0, 0.0).cross(vec3(0.0, 1.0, 0.0)))", "vec3(0.0, 0.0, 1.0)\n");

    prints("print(vec2(1.0, -1.0).reflect(vec2(0.0, 1.0)))", "vec2(1.0, 1.0)\n");
    prints("print(vec2(3.0, 4.0).project(vec2(1.0, 0.0)))", "vec2(3.0, 0.0)\n");
    prints("print(vec2(3.0, 5.0).slide(vec2(1.0, 0.0)))", "vec2(0.0, 5.0)\n");

    prints("print(vec2(3.0, 4.0).perpendicular())", "vec2(-4.0, 3.0)\n");
    prints("print(abs(vec2(1.0, 0.0).angle_to(vec2(0.0, 1.0)) - 1.5707964) < 0.0001)", "true\n");

    prints("print(vec2(30.0, 40.0).clamp_length(5.0, 10.0))", "vec2(6.0, 8.0)\n");

    prints("print(tan(0.0))", "0.0\n");
    prints("print(asin(0.0))", "0.0\n");
    prints("print(acos(1.0))", "0.0\n");
    prints("print(atan(0.0))", "0.0\n");
    prints("print(exp(0.0))\nprint(log(1.0))\nprint(log2(4.0))\nprint(log10(100.0))\nprint(pow(2.0, 3.0))",
           "1.0\n0.0\n2.0\n2.0\n8.0\n");
    prints("print(sign(-5.0))\nprint(sign(0.0))\nprint(sign(3))", "-1.0\n0.0\n1\n");
    prints("print(lerp(10.0, 20.0, 0.5))\nprint(inverse_lerp(10.0, 20.0, 15.0))\nprint(remap(0.0, 10.0, 0.0, 100.0, 5.0))",
           "15.0\n0.5\n50.0\n");
    prints("print(step(5.0, 3.0))\nprint(step(5.0, 6.0))\nprint(smoothstep(0.0, 10.0, 5.0))",
           "0.0\n1.0\n0.5\n");
    prints("print(move_toward(0.0, 10.0, 4.0))\nprint(move_toward(5.0, 6.0, 2.0))",
           "4.0\n6.0\n");
    prints("print(round(rad_to_deg(3.14159265)))\nprint(deg_to_rad(180.0) > 3.14)",
           "180.0\ntrue\n");
    prints("print(wrap(7.0, 0.0, 5.0))", "2.0\n");
    prints("print(round(vec2(10.0, 0.0).slerp(vec2(0.0, 10.0), 0.5).x))\n"
           "print(round(vec2(10.0, 0.0).slerp(vec2(0.0, 10.0), 0.5).y))",
           "7.0\n7.0\n");

    rejects("print(vec4(1.0).cross(vec4(2.0)))", "1:17: 'vec4' has no method 'cross'\n");
    rejects("print(vec3(1.0).perpendicular())", "1:17: 'vec3' has no method 'perpendicular'\n");
    rejects("print(vec3(1.0).angle_to(vec3(2.0)))", "1:17: 'vec3' has no method 'angle_to'\n");
    rejects("print(vec3(1.0).slerp(vec3(2.0), 0.5))", "1:17: 'vec3' has no method 'slerp'\n");
}

TEST_CASE("vec: type errors") {
    rejects("var v = vec2(1.0, 2.0, 3.0)", "1:9: 'vec2' needs 2 components, got 3\n");
    rejects("var i = 1\nvar v = vec2(i, 2.0)",
            "2:14: vector components must be 'float' or vectors, found 'int'; convert with 'as float'\n");
    rejects("var v = vec2(1.0, 2.0) + vec3(1.0)", "1:9: operator '+' is not defined for 'vec2' and 'vec3'\n");
    rejects("var v = vec2(1.0, 2.0) + 1.0", "1:9: operator '+' is not defined for 'vec2' and 'float'\n");
    rejects("var v: ivec2", "1:8: unknown type 'ivec2'\n"); // ivecN are structs of the math prelude (9.21)
    panics("var v = vec3(1.0)\nvar i = 3\nprint(v[i])", "3:7: index 3 is out of bounds for a vec3");
}
