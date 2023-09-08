#include <assert.h>
#include <endian.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>

#include "common.h"
#include "mu.h"

void
message_clear_body(struct message *msg) {
    msg->body_len = 0;
    mu_memzero(msg->body, sizeof(msg->body));
}

void
message_set_body(struct message *msg, const char *value) {
    size_t len;

    len = strlen(value);
    assert(len <= MAX_BODY_SIZE);

    msg->body_len = len;
    mu_strlcpy(msg->body, value, sizeof(msg->body));
}

void
message_set_error(struct message *msg, int err) {
    msg->type = err;
    message_clear_body(msg);
}

ssize_t 
message_deserialize_header(struct message *msg, const uint8_t *buf, size_t size) {
    const uint8_t *p = buf;

    mu_memzero_p(msg);

    if(size < HEADER_SIZE)
        return -ENOMSG;

    memcpy(&msg->id, p, sizeof(msg->id));
    msg->id = be32toh(msg->id);
    p += sizeof(msg->id);

    memcpy(&msg->type, p, sizeof(msg->type));
    msg->type = be16toh(msg->type);
    p += sizeof(msg->type);

    memcpy(&msg->body_len, p, sizeof(msg->body_len));
    msg->body_len = be16toh(msg->body_len);
    p += sizeof(msg->body_len);

    return HEADER_SIZE;
}

ssize_t
message_serialize(struct message *msg, uint8_t *buf, size_t size)
{
    uint8_t *p = buf;
    size_t body_len = msg->body_len;
    uint32_t tmp32;
    uint16_t tmp16;

    assert(body_len <= MAX_BODY_LEN);

    if (size < (HEADER_SIZE + body_len))
        return -ENOMSG;

    tmp32 = htobe32(msg->id);
    memcpy(p, &tmp32, sizeof(tmp32));
    p += sizeof(tmp32);

    tmp16 = htobe16(msg->type);
    memcpy(p, &tmp16, sizeof(tmp16));
    p += sizeof(tmp16);

    tmp16 = htobe16(msg->body_len);
    memcpy(p, &tmp16, sizeof(tmp16));
    p += sizeof(tmp16);

    memcpy(p, msg->body, body_len);

    return (ssize_t)(HEADER_SIZE + body_len);
}

ssize_t
message_deserialize(struct message *msg, const uint8_t *buf, size_t size) {
    const uint8_t *p = buf;
    uint16_t len;

    mu_memzero_p(msg);

    if(size < HEADER_SIZE)
        return -ENOMSG;

    /* deserialize header */
    memcpy(&msg->id, p, sizeof(msg->id));
    msg->id = be32toh(msg->id);
    p += sizeof(msg->id);

    memcpy(&msg->type, p, sizeof(msg->type));
    msg->type = be16toh(msg->type);
    p += sizeof(msg->type);

    memcpy(&len, p, sizeof(len));
    msg->body_len = be16toh(len);
    p += sizeof(msg->body_len);    

    if(msg->body_len > MAX_BODY_LEN)
        return -E2BIG;
    
    /* deserialize body */
    if((HEADER_SIZE + msg->body_len) > size) 
        return -ENOMSG;

    memcpy(msg->body, p, msg->body_len);

    return (ssize_t)(HEADER_SIZE + msg->body_len);
}