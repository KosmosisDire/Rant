/* The C++ standard types with no middleware, and, built with RANT_TEST_AFTER_RANT, after rant.hpp in
 * one file. test.cpp includes them before it, so both orders stay covered. Exit 0 = PASS. */
#ifdef RANT_TEST_AFTER_RANT
#include "rant.hpp"
#endif
#include "rant_types.hpp"

#include <cstdio>
#include <type_traits>

using namespace rant::types;

static int g_failures = 0;
static void chk(const char* what, bool ok) {
    std::printf("  %s  %s\n", ok ? "ok " : "FAIL", what);
    if (!ok) g_failures++;
}

/* The fixed types math and other C libraries take by pointer. */
static_assert(std::is_trivially_copyable<Pose>::value && std::is_standard_layout<Pose>::value, "Pose");
static_assert(std::is_trivially_copyable<CameraIntrinsics>::value && std::is_standard_layout<CameraIntrinsics>::value,
              "CameraIntrinsics");
static_assert(std::is_same<std::underlying_type<ImageFormat>::type, uint8_t>::value, "an enum keeps its backing");
static_assert(std::is_same<std::underlying_type<DistortionModel>::type, uint64_t>::value, "an enum keeps its backing");

int main() {
    Pose pose;
    CameraIntrinsics camera;
    Image image;
    PointField field;
    Empty empty;
    (void)empty;
    chk("a default value is zero", pose.position.x == 0 && pose.orientation.w == 0 && camera.fx == 0 && camera.coeffs[7] == 0);
    chk("an enum starts on its 0 option", camera.model == DistortionModel::None_ && image.format == ImageFormat::Mono8);
    chk("an enum with no 0 option starts on its first", field.kind == ScalarKind::I8);

    Double3 aggregate{ 1.0, 2.0, 3.0 };
    Pose nested{ { 1.0, 2.0, 3.0 }, identity_rotation() };
    chk("a struct stays an aggregate", aggregate.z == 3.0 && nested.position.y == 2.0 && nested.orientation.w == 1.0);

    chk("the identities", identity_pose().orientation.w == 1.0 && identity_transform().pose.orientation.w == 1.0
                          && identity_transform().parent.len == 0);
    Timestamp t{ 5 };
    chk("time arithmetic", (t + Duration{ 2 }).us == 7 && (t - Timestamp{ 2 }).us == 3 && Timestamp{ 2 } < t && t == Timestamp{ 5 });
    chk("color hex", color_to_hex(color_from_hex(0x11223344u)) == 0x11223344u && color_from_hex(0x11223344u).a == 0x44);
    Uuid uuid;
    chk("a default Uuid is nil", is_nil(uuid));
    uuid.bytes[15] = 1;
    chk("and one with a byte set is not", !is_nil(uuid));

    rant::String<4> s("ab");
    chk("a capped string holds a literal", s == "ab" && s.len == 2);
    chk("and refuses past its cap", !s.assign("abcde") && s == "ab");
    Transform frame{ {}, "map" };
    chk("a parent frame from a literal", frame.parent == "map");
    Uri uri;
    chk("Uri holds the wire's capped string", uri.value.assign("rtsp://x") && uri.value == "rtsp://x");

    std::printf(g_failures ? "FAILED\n" : "PASS\n");
    return g_failures ? 1 : 0;
}
