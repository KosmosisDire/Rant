/* The DATA, HB, NACK and SHM-DATA submessage builders. */
#include "internal.h"


size_t i_dart_wire_mk_data(uint8_t *o, uint16_t index, uint64_t seqno, i_DartWriterSample *s,
                         uint16_t frag, const uint8_t *payload, uint16_t payload_len){
    i_dart_le_w16(o+DART_OFFSET_INDEX,index);
    if (s->count==1){                       /* frag 0, count 1 and len are implied */
        o[0]=(uint8_t)(DART_DATA|DART_F_SINGLE);
        i_dart_le_w64(o+DART_OFFSET_SEQNO,seqno); i_dart_le_w16(o+DART_OFFSET_PAYLOAD_LEN_SINGLE,payload_len);
        memcpy(o+DART_HEADER_DATA_SINGLE,payload,payload_len);
        return DART_HEADER_DATA_SINGLE+payload_len;
    }
    o[0]=DART_DATA;
    i_dart_le_w64(o+DART_OFFSET_SEQNO,seqno); i_dart_le_w16(o+DART_OFFSET_FRAG,frag); i_dart_le_w16(o+DART_OFFSET_COUNT,s->count);
    i_dart_le_w32(o+DART_OFFSET_SAMPLE_LEN,s->len); i_dart_le_w16(o+DART_OFFSET_PAYLOAD_LEN,payload_len);
    memcpy(o+DART_HEADER_DATA_MULTI,payload,payload_len);
    return DART_HEADER_DATA_MULTI+payload_len;
}

#ifdef DART_SHM
/* One submessage covers [base, base+count). The body is the descriptor, no payload. */
size_t i_dart_wire_mk_shm(uint8_t *o, uint16_t index, uint64_t base, uint16_t count,
                          const uint8_t *desc){
    o[0]=(uint8_t)(DART_DATA|DART_F_SHM); i_dart_le_w16(o+DART_OFFSET_INDEX,index);
    i_dart_le_w64(o+DART_OFFSET_SEQNO,base); i_dart_le_w16(o+DART_OFFSET_SHM_COUNT,count);
    memcpy(o+DART_OFFSET_SHM_DESC,desc,DART_SHM_DESC_BYTES);
    return DART_SHM_DATA_BYTES;
}
#endif

size_t i_dart_wire_mk_hb(uint8_t *o, uint16_t index, uint64_t first, uint64_t last, uint32_t cnt){
    o[0]=DART_HB; i_dart_le_w16(o+DART_OFFSET_INDEX,index); i_dart_le_w64(o+DART_OFFSET_SEQNO,first); i_dart_le_w64(o+DART_OFFSET_HB_LAST,last); i_dart_le_w32(o+DART_OFFSET_HB_COUNT,cnt);
    return DART_HEADER_HB;
}

size_t i_dart_wire_mk_nack(uint8_t *o, uint16_t index, uint64_t base, uint16_t nbits, uint32_t bitmap,
                         uint32_t epoch, uint8_t flags){
    o[0]=(uint8_t)(DART_NACK|flags); i_dart_le_w16(o+DART_OFFSET_INDEX,index); i_dart_le_w64(o+DART_OFFSET_SEQNO,base);
    i_dart_le_w16(o+DART_OFFSET_NACK_NBITS,nbits); i_dart_le_w32(o+DART_OFFSET_NACK_BITMAP,bitmap); i_dart_le_w32(o+DART_OFFSET_NACK_EPOCH,epoch);
    return DART_HEADER_NACK;
}
