
/* What the schema cannot say, pasted after the generated constructors of rant_types.h. */
static inline RantQuaternion rant_quaternion_identity(void){
    return rant_quaternion(0.0, 0.0, 0.0, 1.0);
}
static inline RantPose rant_pose_identity(void){
    RantPose p; p.position = rant_double3(0.0, 0.0, 0.0);
    p.orientation = rant_quaternion_identity(); return p;
}
static inline RantTransform rant_transform_identity(void){
    RantTransform t;
    int i;
    t.pose = rant_pose_identity();
    t.parent_len = 0;
    for (i = 0; i < 30; i++) t.parent[i] = 0;   /* the parent name reaches the wire, so zero all of it */
    return t;
}
/* 0xRRGGBBAA both ways, the CSS order with the alpha last. */
static inline RantColor rant_color_from_hex(uint32_t rgba){
    return rant_color((uint8_t)(rgba >> 24), (uint8_t)(rgba >> 16),
                      (uint8_t)(rgba >> 8),  (uint8_t)rgba);
}
static inline uint32_t rant_color_to_hex(RantColor c){
    return ((uint32_t)c.r << 24) | ((uint32_t)c.g << 16) | ((uint32_t)c.b << 8) | c.a;
}
/* Timestamp arithmetic in microseconds. */
static inline RantDuration    rant_timestamp_diff(RantTimestamp a, RantTimestamp b){ return a - b; }
static inline RantTimestamp rant_timestamp_add (RantTimestamp t, RantDuration d){ return t + d; }
static inline RantDuration    rant_duration_ms     (int64_t ms){ return ms * 1000; }
static inline RantDuration    rant_duration_s      (double s){ return (int64_t)(s * 1000000.0); }
static inline double          rant_duration_to_s (RantDuration d){ return (double)d / 1000000.0; }
/* All zeroes, the conventional unset. */
static inline int rant_uuid_is_nil(RantUuid u){
    int i; for (i = 0; i < 16; i++) if (u.bytes[i]) return 0;
    return 1;
}
