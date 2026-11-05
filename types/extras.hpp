/* What the schema cannot say in C++, around the generated structs at the marker. */
#include <stddef.h>
#include <stdint.h>
#include <array>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace rant {

/* The capped string wire slot, [u16 live length][N bytes]. assign() refuses over capacity
 * rather than truncating, view() clamps a hostile length. */
template <uint16_t N> struct String {
    uint16_t len = 0;
    char     data[N] = {};

    String() = default;
    template <size_t M> String(const char (&lit)[M]) {
        static_assert(M - 1 <= N, "string literal exceeds rant::String capacity");
        len = static_cast<uint16_t>(M - 1);
        std::memcpy(data, lit, M - 1);
    }
    bool assign(std::string_view s) {
        if (s.size() > N) return false;
        len = static_cast<uint16_t>(s.size());
        if (s.size()) std::memcpy(data, s.data(), s.size());
        if (s.size() < N) std::memset(data + s.size(), 0, N - s.size());
        return true;
    }
    std::string_view view() const { return { data, len <= N ? len : N }; }
    bool operator==(std::string_view s) const { return view() == s; }
};

/* Apart from the root so Color or Quaternion never clashes with an engine type under a
 * using directive. */
namespace types {

/* @TYPES@ */

/* The rotation that turns nothing, unlike the zero a default Quaternion holds. */
inline Quaternion identity_rotation() { return { 0.0, 0.0, 0.0, 1.0 }; }
inline Pose identity_pose() { return { {}, identity_rotation() }; }
inline Transform identity_transform() { return { identity_pose(), {} }; }

inline bool operator==(Timestamp a, Timestamp b) { return a.us == b.us; }
inline bool operator<(Timestamp a, Timestamp b) { return a.us < b.us; }
inline Duration  operator-(Timestamp a, Timestamp b) { return Duration{ a.us - b.us }; }
inline Timestamp operator+(Timestamp t, Duration d) { return Timestamp{ t.us + d.us }; }
inline bool operator==(Duration a, Duration b) { return a.us == b.us; }

/* 0xRRGGBBAA both ways, the order you read in CSS */
inline Color color_from_hex(uint32_t rgba) {
    return Color{ (uint8_t)(rgba >> 24), (uint8_t)(rgba >> 16), (uint8_t)(rgba >> 8), (uint8_t)rgba };
}
inline uint32_t color_to_hex(Color c) {
    return ((uint32_t)c.r << 24) | ((uint32_t)c.g << 16) | ((uint32_t)c.b << 8) | c.a;
}

inline bool is_nil(const Uuid& u) {
    for (int i = 0; i < 16; i++) if (u.bytes[i]) return false;
    return true;
}

}   /* namespace types */
}   /* namespace rant */
