#include <doctest.h>

#include <string>

#include "cuervo/core/name.hpp"

using namespace cuervo;

TEST_CASE("Names intern to the same id") {
    Name a("shoot");
    Name b(std::string("sho") + "ot");
    Name c("focus");
    CHECK(a == b);
    CHECK_FALSE(a == c);
    CHECK(a.str() == "shoot");
    CHECK("shoot"_name == a);
    CHECK(Name().empty());
    CHECK(Name("") == Name());
}
