// A freestanding C++26 smoke project: no SDK or runtime download is needed.
// Larger projects can explicitly import their installed SDK/STL/packages.
constexpr int square(int value) { return value * value; }
static_assert(square(7) == 49);
// C++26 pack indexing makes a silently older language mode fail this build.
template <auto... values> constexpr auto first_value = values...[0];
static_assert(first_value<81, 9, 0> == 81);
extern "C" int mainCRTStartup() {
    return square(9) == first_value<81, 9, 0> ? 0 : 1;
}
