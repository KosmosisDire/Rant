/* Transport wire codec: the DATA/HB/NACK[/SHM-DATA] submessage builders. */
#include "internal.h"


/* Submessage wire layout. Byte 0 = type|flags, bytes 1-2 = alias, then the body.
 * Builders (dart_mk_*) and the readers both index off these, so moving a field is one
 * edit, never a silent builder/parser drift. Several fields share an offset (distinct
 * names on purpose). Header sizes: DATA 13 (single)/21 (multi), HB 23, NACK 21. */

/* datagram builders (return length) */
size_t dart_mk_data(uint8_t *o, uint16_t alias, uint64_t seqno, dart_writer_sample *s,
                         uint16_t frag, const uint8_t *payload, uint16_t payload_len){
    dart_le_w16(o+DART_OFFSET_ALIAS,alias);
    if (s->count==1){                       /* frag=0, count=1, len=payload_len implied */
        o[0]=(uint8_t)(DART_DATA|DART_F_SINGLE);
        dart_le_w64(o+DART_OFFSET_SEQNO,seqno); dart_le_w16(o+DART_OFFSET_PAYLOAD_LEN_SINGLE,payload_len);
        memcpy(o+DART_HEADER_DATA_SINGLE,payload,payload_len);
        return DART_HEADER_DATA_SINGLE+payload_len;
    }
    o[0]=DART_DATA;
    dart_le_w64(o+DART_OFFSET_SEQNO,seqno); dart_le_w16(o+DART_OFFSET_FRAG,frag); dart_le_w16(o+DART_OFFSET_COUNT,s->count);
    dart_le_w32(o+DART_OFFSET_SAMPLE_LEN,s->len); dart_le_w16(o+DART_OFFSET_PAYLOAD_LEN,payload_len);
    memcpy(o+DART_HEADER_DATA_MULTI,payload,payload_len);
    return DART_HEADER_DATA_MULTI+payload_len;
}

#ifdef DART_SHM
/* SHM-DATA: one submessage covers [base, base+count); body is the descriptor, no
 * payload. 37 bytes = 1 (type|F_SHM) + 2 (alias) + 8 (base) + 2 (count) + 24 (desc). */
size_t dart_mk_shm(uint8_t *o, uint16_t alias, uint64_t base, uint16_t count,
                          const uint8_t *desc){
    o[0]=(uint8_t)(DART_DATA|DART_F_SHM); dart_le_w16(o+DART_OFFSET_ALIAS,alias);
    dart_le_w64(o+DART_OFFSET_SEQNO,base); dart_le_w16(o+DART_OFFSET_SHM_COUNT,count);
    memcpy(o+DART_OFFSET_SHM_DESC,desc,DART_SHM_DESC_BYTES);
    return DART_SHM_DATA_BYTES;
}
#endif

size_t dart_mk_hb(uint8_t *o, uint16_t alias, uint64_t first, uint64_t last, uint32_t cnt){
    o[0]=DART_HB; dart_le_w16(o+DART_OFFSET_ALIAS,alias); dart_le_w64(o+DART_OFFSET_SEQNO,first); dart_le_w64(o+DART_OFFSET_HB_LAST,last); dart_le_w32(o+DART_OFFSET_HB_COUNT,cnt);
    return DART_HEADER_HB;
}

size_t dart_mk_nack(uint8_t *o, uint16_t alias, uint64_t base, uint16_t nbits, uint32_t bitmap,
                         uint32_t epoch, uint8_t flags){
    o[0]=(uint8_t)(DART_NACK|flags); dart_le_w16(o+DART_OFFSET_ALIAS,alias); dart_le_w64(o+DART_OFFSET_SEQNO,base);
    dart_le_w16(o+DART_OFFSET_NACK_NBITS,nbits); dart_le_w32(o+DART_OFFSET_NACK_BITMAP,bitmap); dart_le_w32(o+DART_OFFSET_NACK_EPOCH,epoch);
    return DART_HEADER_NACK;
}
