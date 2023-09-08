#ifndef _COMMON_H_
#define _COMMON_H_

#include <sys/types.h>

#include <stddef.h>
#include <stdint.h>

#define DEFAULT_PORT_STR "9514"

#define QTYPE_A  1U     /* lookup IP for domain */
#define QTYPE_PTR  12U  /* lookup domain(s) for IP */ 

#define RCODE_NOERROR 0U    /* no errror */
#define RCODE_FORMERR 1U    /* request format error */
#define RCODE_NXDOMAIN 3U   /* non-existent domain */

#define MAX_DOMAIN_LEN     253U    /* max strlen */
#define MAX_DOMAIN_SIZE    254U    /* includes nul */

#define HEADER_SIZE 8U
#define MAX_BODY_LEN      MAX_DOMAIN_LEN
#define MAX_BODY_SIZE     MAX_DOMAIN_SIZE /* includes nul */
#define MAX_MESSAGE_SIZE (HEADER_SIZE + MAX_BODY_LEN)


struct message {
    /* header */
    uint32_t id;
    uint16_t type;  /* for requests, qtype; for responses, rcode */
    uint16_t body_len;

    /* body */
    /* when deserialized, includes nul; when serialized, does not include nul */
    char body[MAX_BODY_SIZE];
};

void message_clear_body(struct message *msg);
void message_set_body(struct message *msg, const char *value);
void message_set_error(struct message *msg, int err);
ssize_t message_deserialize_header(struct message *msg, const uint8_t *buf, size_t size);
ssize_t message_serialize(struct message *msg, uint8_t *buf, size_t size);
ssize_t message_deserialize(struct message *msg, const uint8_t *buf, size_t size);

#endif /* _COMMON_H_ */
