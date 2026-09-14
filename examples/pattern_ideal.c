



static void add_handler(const RambleReq *req)
{
    if (req->schema)
    {
        int64_t x = ramble_get_int(req->data, req->schema, "x");
        int64_t y = ramble_get_int(req->data, req->schema, "y");

        printf("  compute/add request: x=%lld, y=%lld\n", (long long)x, (long long)y);
        int64_t sum = x + y;

        uint8_t buf[4]; size_t cap = sizeof buf; // one 32 bit integer
        ramble_schema_message_default(g_schema, buf, cap);
        ramble_set_int (buf, cap, g_schema, "z", (int32_t)sum);

        ramble_call_reply(req, ramble_bytes(buf, cap));
    }
}