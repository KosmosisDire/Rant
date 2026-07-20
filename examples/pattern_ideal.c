



static void add_handler(const DartReq *req)
{
    if (req->schema)
    {
        int64_t x = dart_get_int(req->data, req->schema, "x");
        int64_t y = dart_get_int(req->data, req->schema, "y");

        printf("  compute/add request: x=%lld, y=%lld\n", (long long)x, (long long)y);
        int64_t sum = x + y;

        uint8_t buf[4]; size_t cap = sizeof buf; // one 32 bit integer
        dart_schema_message_default(g_schema, buf, cap);
        dart_set_int (buf, cap, g_schema, "z", (int32_t)sum);

        dart_call_reply(req, dart_bytes(buf, cap));
    }
}