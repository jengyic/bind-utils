/*
 * Copyright (C) 1999  Internet Software Consortium.
 * 
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS" AND INTERNET SOFTWARE CONSORTIUM DISCLAIMS
 * ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL INTERNET SOFTWARE
 * CONSORTIUM BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS
 * ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
 * SOFTWARE.
 */

/***
 *** Imports
 ***/

#include <config.h>

#include <stddef.h>
#include <string.h>

#include <isc/assertions.h>
#include <isc/boolean.h>
#include <isc/region.h>
#include <isc/types.h>

#include <dns/message.h>
#include <dns/rdataset.h>
#include <dns/rdata.h>
#include <dns/rdataclass.h>
#include <dns/rdatatype.h>
#include <dns/rdatalist.h>
#include <dns/compress.h>
#include <dns/tsig.h>

#define DNS_MESSAGE_OPCODE_MASK		0x7800U
#define DNS_MESSAGE_OPCODE_SHIFT	11
#define DNS_MESSAGE_RCODE_MASK		0x000fU
#define DNS_MESSAGE_FLAG_MASK		0x8ff0U
#define DNS_MESSAGE_EDNSRCODE_MASK	0xff000000U
#define DNS_MESSAGE_EDNSRCODE_SHIFT	24
#define DNS_MESSAGE_EDNSVERSION_MASK	0x00ff0000U
#define DNS_MESSAGE_EDNSVERSION_SHIFT	16

#define VALID_NAMED_SECTION(s)  (((s) > DNS_SECTION_ANY) \
				 && ((s) < DNS_SECTION_MAX))
#define VALID_SECTION(s)	(((s) >= DNS_SECTION_ANY) \
				 && ((s) < DNS_SECTION_MAX))

/*
 * This is the size of each individual scratchpad buffer, and the numbers
 * of various block allocations used within the server.
 * XXXMLG These should come from a config setting.
 */
#define SCRATCHPAD_SIZE		512
#define NAME_COUNT		  8
#define RDATA_COUNT		  8
#define RDATALIST_COUNT		  8
#define RDATASET_COUNT		 RDATALIST_COUNT

/*
 * "helper" type, which consists of a block of some type, and is linkable.
 * For it to work, sizeof(dns_msgblock_t) must be a multiple of the pointer
 * size, or the allocated elements will not be alligned correctly.
 */
struct dns_msgblock {
	unsigned int			count;
	unsigned int			remaining;
	ISC_LINK(dns_msgblock_t)	link;
}; /* dynamically sized */

static inline dns_msgblock_t *
msgblock_allocate(isc_mem_t *, unsigned int, unsigned int);

#define msgblock_get(block, type) \
	((type *)msgblock_internalget(block, sizeof(type)))

static inline void *
msgblock_internalget(dns_msgblock_t *, unsigned int);

static inline void
msgblock_reset(dns_msgblock_t *);

static inline void
msgblock_free(isc_mem_t *, dns_msgblock_t *, unsigned int);

/*
 * Allocate a new dns_msgblock_t, and return a pointer to it.  If no memory
 * is free, return NULL.
 */
static inline dns_msgblock_t *
msgblock_allocate(isc_mem_t *mctx, unsigned int sizeof_type,
		  unsigned int count)
{
	dns_msgblock_t *block;
	unsigned int length;

	length = sizeof(dns_msgblock_t) + (sizeof_type * count);

	block = isc_mem_get(mctx, length);
	if (block == NULL)
		return (NULL);

	block->count = count;
	block->remaining = count;

	ISC_LINK_INIT(block, link);

	return (block);
}

/*
 * Return an element from the msgblock.  If no more are available, return
 * NULL.
 */
static inline void *
msgblock_internalget(dns_msgblock_t *block, unsigned int sizeof_type)
{
	void *ptr;

	if (block == NULL || block->remaining == 0)
		return (NULL);

	block->remaining--;

	ptr = (((unsigned char *)block)
	       + sizeof(dns_msgblock_t)
	       + (sizeof_type * block->remaining));

	return (ptr);
}

static inline void
msgblock_reset(dns_msgblock_t *block)
{
	block->remaining = block->count;
}

/*
 * Release memory associated with a message block.
 */
static inline void
msgblock_free(isc_mem_t *mctx, dns_msgblock_t *block,
	      unsigned int sizeof_type)
{
	unsigned int length;

	length = sizeof(dns_msgblock_t) + (sizeof_type * block->count);

	isc_mem_put(mctx, block, length);
}

/*
 * Allocate a new dynamic buffer, and attach it to this message as the
 * "current" buffer.  (which is always the last on the list, for our
 * uses)
 */
static inline dns_result_t
newbuffer(dns_message_t *msg, unsigned int size)
{
	isc_result_t result;
	isc_buffer_t *dynbuf;

	dynbuf = NULL;
	result = isc_buffer_allocate(msg->mctx, &dynbuf, size,
					ISC_BUFFERTYPE_BINARY);
	if (result != ISC_R_SUCCESS)
		return (DNS_R_NOMEMORY);

	ISC_LIST_APPEND(msg->scratchpad, dynbuf, link);
	return (DNS_R_SUCCESS);
}

static inline isc_buffer_t *
currentbuffer(dns_message_t *msg)
{
	isc_buffer_t *dynbuf;

	dynbuf = ISC_LIST_TAIL(msg->scratchpad);
	INSIST(dynbuf != NULL);

	return (dynbuf);
}

static inline void
releasename(dns_message_t *msg, dns_name_t *name)
{
	ISC_LIST_PREPEND(msg->freename, name, link);
}

static inline dns_name_t *
newname(dns_message_t *msg)
{
	dns_msgblock_t *msgblock;
	dns_name_t *name;

	name = ISC_LIST_HEAD(msg->freename);
	if (name != NULL) {
		ISC_LIST_UNLINK(msg->freename, name, link);
		return (name);
	}

	msgblock = ISC_LIST_TAIL(msg->names);
	name = msgblock_get(msgblock, dns_name_t);
	if (name == NULL) {
		msgblock = msgblock_allocate(msg->mctx, sizeof(dns_name_t),
					     NAME_COUNT);
		if (msgblock == NULL)
			return (NULL);

		ISC_LIST_APPEND(msg->names, msgblock, link);

		name = msgblock_get(msgblock, dns_name_t);
	}

	return (name);
}

static inline void
releaserdata(dns_message_t *msg, dns_rdata_t *rdata)
{
	ISC_LIST_PREPEND(msg->freerdata, rdata, link);
}

static inline dns_rdata_t *
newrdata(dns_message_t *msg)
{
	dns_msgblock_t *msgblock;
	dns_rdata_t *rdata;

	rdata = ISC_LIST_HEAD(msg->freerdata);
	if (rdata != NULL) {
		ISC_LIST_UNLINK(msg->freerdata, rdata, link);
		return (rdata);
	}

	msgblock = ISC_LIST_TAIL(msg->rdatas);
	rdata = msgblock_get(msgblock, dns_rdata_t);
	if (rdata == NULL) {
		msgblock = msgblock_allocate(msg->mctx, sizeof(dns_rdata_t),
					     RDATA_COUNT);
		if (msgblock == NULL)
			return (NULL);

		ISC_LIST_APPEND(msg->rdatas, msgblock, link);

		rdata = msgblock_get(msgblock, dns_rdata_t);
	}

	return (rdata);
}

static inline void
releaserdatalist(dns_message_t *msg, dns_rdatalist_t *rdatalist)
{
	ISC_LIST_PREPEND(msg->freerdatalist, rdatalist, link);
}

static inline dns_rdatalist_t *
newrdatalist(dns_message_t *msg)
{
	dns_msgblock_t *msgblock;
	dns_rdatalist_t *rdatalist;

	rdatalist = ISC_LIST_HEAD(msg->freerdatalist);
	if (rdatalist != NULL) {
		ISC_LIST_UNLINK(msg->freerdatalist, rdatalist, link);
		return (rdatalist);
	}

	msgblock = ISC_LIST_TAIL(msg->rdatalists);
	rdatalist = msgblock_get(msgblock, dns_rdatalist_t);
	if (rdatalist == NULL) {
		msgblock = msgblock_allocate(msg->mctx,
					     sizeof(dns_rdatalist_t),
					     RDATALIST_COUNT);
		if (msgblock == NULL)
			return (NULL);

		ISC_LIST_APPEND(msg->rdatalists, msgblock, link);

		rdatalist = msgblock_get(msgblock, dns_rdatalist_t);
	}

	return (rdatalist);
}

static inline void
releaserdataset(dns_message_t *msg, dns_rdataset_t *rdataset)
{
	ISC_LIST_PREPEND(msg->freerdataset, rdataset, link);
}

static inline dns_rdataset_t *
newrdataset(dns_message_t *msg)
{
	dns_msgblock_t *msgblock;
	dns_rdataset_t *rdataset;

	rdataset = ISC_LIST_HEAD(msg->freerdataset);
	if (rdataset != NULL) {
		ISC_LIST_UNLINK(msg->freerdataset, rdataset, link);
		return (rdataset);
	}

	msgblock = ISC_LIST_TAIL(msg->rdatasets);
	rdataset = msgblock_get(msgblock, dns_rdataset_t);
	if (rdataset == NULL) {
		msgblock = msgblock_allocate(msg->mctx, sizeof(dns_rdataset_t),
					     RDATASET_COUNT);
		if (msgblock == NULL)
			return (NULL);

		ISC_LIST_APPEND(msg->rdatasets, msgblock, link);

		rdataset = msgblock_get(msgblock, dns_rdataset_t);
	}

	return (rdataset);
}

static inline void
msginitheader(dns_message_t *m)
{
	m->id = 0;
	m->flags = 0;
	m->rcode = 0;
	m->opcode = 0;
	m->rdclass = 0;
}

static inline void
msginitprivate(dns_message_t *m)
{
	unsigned int i;

	for (i = 0; i < DNS_SECTION_MAX; i++) {
		m->cursors[i] = NULL;
		m->counts[i] = 0;
	}
	m->opt = NULL;
	m->state = DNS_SECTION_ANY;  /* indicate nothing parsed or rendered */
	m->reserved = 0;
	m->buffer = NULL;
	m->need_cctx_cleanup = 0;
}

static inline void
msginittsig(dns_message_t *m)
{
	m->tsigstatus = m->querytsigstatus = dns_rcode_noerror;
	m->tsig = m->querytsig = NULL;
	m->tsigkey = NULL;
	m->tsigstart = -1;
}

/*
 * Init elements to default state.  Used both when allocating a new element
 * and when resetting one.
 */
static inline void
msginit(dns_message_t *m)
{
	msginitheader(m);
	msginitprivate(m);
	msginittsig(m);
	m->header_ok = 0;
	m->question_ok = 0;
}

static inline void
msgresetnames(dns_message_t *msg, unsigned int first_section) {
	unsigned int i;
	dns_name_t *name, *next_name;
	dns_rdataset_t *rds, *next_rds;

	/*
	 * Clean up name lists by calling the rdataset disassociate function.
	 */
	for (i = first_section; i < DNS_SECTION_MAX; i++) {
		name = ISC_LIST_HEAD(msg->sections[i]);
		while (name != NULL) {
			next_name = ISC_LIST_NEXT(name, link);
			ISC_LIST_UNLINK(msg->sections[i], name, link);

			rds = ISC_LIST_HEAD(name->list);
			while (rds != NULL) {
				next_rds = ISC_LIST_NEXT(rds, link);
				ISC_LIST_UNLINK(name->list, rds, link);

				dns_rdataset_disassociate(rds);
				rds = next_rds;
			}
			name = next_name;
		}
	}
}

/*
 * Free all but one (or everything) for this message.  This is used by
 * both dns_message_reset() and dns_message_parse().
 */
static void
msgreset(dns_message_t *msg, isc_boolean_t everything)
{
	dns_msgblock_t *msgblock, *next_msgblock;
	isc_buffer_t *dynbuf, *next_dynbuf;
	dns_rdataset_t *rds;
	dns_name_t *name;
	dns_rdata_t *rdata;
	dns_rdatalist_t *rdatalist;

	msgresetnames(msg, 0);

	if (msg->opt != NULL)
		dns_rdataset_disassociate(msg->opt);
	msg->opt = NULL;

	/*
	 * Clean up linked lists.
	 */

	/*
	 * Run through the free lists, and just unlink anything found there.
	 * The memory isn't lost since these are part of message blocks we
	 * have allocated.
	 */
	name = ISC_LIST_HEAD(msg->freename);
	while (name != NULL) {
		ISC_LIST_UNLINK(msg->freename, name, link);
		name = ISC_LIST_HEAD(msg->freename);
	}
	rdata = ISC_LIST_HEAD(msg->freerdata);
	while (rdata != NULL) {
		ISC_LIST_UNLINK(msg->freerdata, rdata, link);
		rdata = ISC_LIST_HEAD(msg->freerdata);
	}
	rdatalist = ISC_LIST_HEAD(msg->freerdatalist);
	while (rdatalist != NULL) {
		ISC_LIST_UNLINK(msg->freerdatalist, rdatalist, link);
		rdatalist = ISC_LIST_HEAD(msg->freerdatalist);
	}
	rds = ISC_LIST_HEAD(msg->freerdataset);
	while (rds != NULL) {
		ISC_LIST_UNLINK(msg->freerdataset, rds, link);
		rds = ISC_LIST_HEAD(msg->freerdataset);
	}

	dynbuf = ISC_LIST_HEAD(msg->scratchpad);
	INSIST(dynbuf != NULL);
	if (!everything) {
		isc_buffer_clear(dynbuf);
		dynbuf = ISC_LIST_NEXT(dynbuf, link);
	}
	while (dynbuf != NULL) {
		next_dynbuf = ISC_LIST_NEXT(dynbuf, link);
		ISC_LIST_UNLINK(msg->scratchpad, dynbuf, link);
		isc_buffer_free(&dynbuf);
		dynbuf = next_dynbuf;
	}

	msgblock = ISC_LIST_HEAD(msg->names);
	INSIST(msgblock != NULL);
	if (!everything) {
		msgblock_reset(msgblock);
		msgblock = ISC_LIST_NEXT(msgblock, link);
	}
	while (msgblock != NULL) {
		next_msgblock = ISC_LIST_NEXT(msgblock, link);
		ISC_LIST_UNLINK(msg->names, msgblock, link);
		msgblock_free(msg->mctx, msgblock, sizeof(dns_name_t));
		msgblock = next_msgblock;
	}

	msgblock = ISC_LIST_HEAD(msg->rdatas);
	INSIST(msgblock != NULL);
	if (!everything) {
		msgblock_reset(msgblock);
		msgblock = ISC_LIST_NEXT(msgblock, link);
	}
	while (msgblock != NULL) {
		next_msgblock = ISC_LIST_NEXT(msgblock, link);
		ISC_LIST_UNLINK(msg->rdatas, msgblock, link);
		msgblock_free(msg->mctx, msgblock, sizeof(dns_rdata_t));
		msgblock = next_msgblock;
	}

	msgblock = ISC_LIST_HEAD(msg->rdatasets);
	INSIST(msgblock != NULL);
	if (!everything) {
		msgblock_reset(msgblock);
		msgblock = ISC_LIST_NEXT(msgblock, link);
	}
	while (msgblock != NULL) {
		next_msgblock = ISC_LIST_NEXT(msgblock, link);
		ISC_LIST_UNLINK(msg->rdatasets, msgblock, link);
		msgblock_free(msg->mctx, msgblock, sizeof(dns_rdataset_t));
		msgblock = next_msgblock;
	}

	/*
	 * rdatalists could be empty.
	 */

	msgblock = ISC_LIST_HEAD(msg->rdatalists);
	if (!everything && msgblock != NULL) {
		msgblock_reset(msgblock);
		msgblock = ISC_LIST_NEXT(msgblock, link);
	}
	while (msgblock != NULL) {
		next_msgblock = ISC_LIST_NEXT(msgblock, link);
		ISC_LIST_UNLINK(msg->rdatalists, msgblock, link);
		msgblock_free(msg->mctx, msgblock, sizeof(dns_rdatalist_t));
		msgblock = next_msgblock;
	}

	if (msg->need_cctx_cleanup == 1)
		dns_compress_invalidate(&msg->cctx);

	if (msg->tsig != NULL) {
		dns_rdata_freestruct(msg->tsig);
		isc_mem_put(msg->mctx, msg->tsig,
			    sizeof(dns_rdata_any_tsig_t));
	}

	if (msg->querytsig != NULL) {
		dns_rdata_freestruct(msg->querytsig);
		isc_mem_put(msg->mctx, msg->querytsig,
			    sizeof(dns_rdata_any_tsig_t));
        }

	if (msg->tsigkey != NULL && dns_tsig_emptykey(msg->tsigkey))
		dns_tsig_key_free(&msg->tsigkey);

	/*
	 * Set other bits to normal default values.
	 */
	if (!everything)
		msginit(msg);
}

dns_result_t
dns_message_create(isc_mem_t *mctx, unsigned int intent, dns_message_t **msgp)
{
	dns_message_t *m;
	isc_result_t iresult;
	dns_msgblock_t *msgblock;
	isc_buffer_t *dynbuf;
	unsigned int i;

	REQUIRE(mctx != NULL);
	REQUIRE(msgp != NULL);
	REQUIRE(*msgp == NULL);
	REQUIRE(intent == DNS_MESSAGE_INTENTPARSE
		|| intent == DNS_MESSAGE_INTENTRENDER);

	m = isc_mem_get(mctx, sizeof(dns_message_t));
	if (m == NULL)
		return(DNS_R_NOMEMORY);

	m->magic = DNS_MESSAGE_MAGIC;
	m->from_to_wire = intent;
	msginit(m);
	for (i = 0 ; i < DNS_SECTION_MAX ; i++)
		ISC_LIST_INIT(m->sections[i]);
	m->mctx = mctx;
	ISC_LIST_INIT(m->scratchpad);
	ISC_LIST_INIT(m->names);
	ISC_LIST_INIT(m->rdatas);
	ISC_LIST_INIT(m->rdatasets);
	ISC_LIST_INIT(m->rdatalists);
	ISC_LIST_INIT(m->freename);
	ISC_LIST_INIT(m->freerdata);
	ISC_LIST_INIT(m->freerdataset);
	ISC_LIST_INIT(m->freerdatalist);

	dynbuf = NULL;
	iresult = isc_buffer_allocate(mctx, &dynbuf, SCRATCHPAD_SIZE,
				      ISC_BUFFERTYPE_BINARY);
	if (iresult != ISC_R_SUCCESS)
		goto cleanup1;
	ISC_LIST_APPEND(m->scratchpad, dynbuf, link);

	msgblock = msgblock_allocate(mctx, sizeof(dns_name_t),
				     NAME_COUNT);
	if (msgblock == NULL)
		goto cleanup2;
	ISC_LIST_APPEND(m->names, msgblock, link);

	msgblock = msgblock_allocate(mctx, sizeof(dns_rdata_t),
				     RDATA_COUNT);
	if (msgblock == NULL)
		goto cleanup3;
	ISC_LIST_APPEND(m->rdatas, msgblock, link);

	msgblock = msgblock_allocate(mctx, sizeof(dns_rdataset_t),
				     RDATASET_COUNT);
	if (msgblock == NULL)
		goto cleanup4;
	ISC_LIST_APPEND(m->rdatasets, msgblock, link);

	if (intent == DNS_MESSAGE_INTENTPARSE) {
		msgblock = msgblock_allocate(mctx, sizeof(dns_rdatalist_t),
					     RDATALIST_COUNT);
		if (msgblock == NULL)
			goto cleanup5;
		ISC_LIST_APPEND(m->rdatalists, msgblock, link);
	}

	*msgp = m;
	return (DNS_R_SUCCESS);

	/*
	 * Cleanup for error returns.
	 */
 cleanup5:
	msgblock = ISC_LIST_HEAD(m->rdatasets);
	msgblock_free(mctx, msgblock, sizeof(dns_rdataset_t));
 cleanup4:
	msgblock = ISC_LIST_HEAD(m->rdatas);
	msgblock_free(mctx, msgblock, sizeof(dns_rdata_t));
 cleanup3:
	msgblock = ISC_LIST_HEAD(m->names);
	msgblock_free(mctx, msgblock, sizeof(dns_name_t));
 cleanup2:
	dynbuf = ISC_LIST_HEAD(m->scratchpad);
	isc_buffer_free(&dynbuf);
 cleanup1:
	m->magic = 0;
	isc_mem_put(mctx, m, sizeof(dns_message_t));

	return (DNS_R_NOMEMORY);
}

void
dns_message_reset(dns_message_t *msg, unsigned int intent)
{
	REQUIRE(DNS_MESSAGE_VALID(msg));
	REQUIRE(intent == DNS_MESSAGE_INTENTPARSE
		|| intent == DNS_MESSAGE_INTENTRENDER);

	msg->from_to_wire = intent;
	msgreset(msg, ISC_FALSE);
}

void
dns_message_destroy(dns_message_t **msgp)
{
	dns_message_t *msg;

	REQUIRE(msgp != NULL);
	REQUIRE(DNS_MESSAGE_VALID(*msgp));

	msg = *msgp;
	*msgp = NULL;

	msgreset(msg, ISC_TRUE);
	msg->magic = 0;
	isc_mem_put(msg->mctx, msg, sizeof(dns_message_t));
}

static dns_result_t
findname(dns_name_t **foundname, dns_name_t *target, dns_namelist_t *section)
{
	dns_name_t *curr;

	for (curr = ISC_LIST_TAIL(*section) ;
	     curr != NULL ;
	     curr = ISC_LIST_PREV(curr, link)) {
		if (dns_name_equal(curr, target)) {
			if (foundname != NULL)
				*foundname = curr;
			return (DNS_R_SUCCESS);
		}
	}

	return (DNS_R_NOTFOUND);
}

static dns_result_t
findtype(dns_rdataset_t **rdataset, dns_name_t *name, dns_rdatatype_t type,
	 dns_rdatatype_t covers)
{
	dns_rdataset_t *curr;

	for (curr = ISC_LIST_TAIL(name->list) ;
	     curr != NULL ;
	     curr = ISC_LIST_PREV(curr, link)) {
		if (curr->type == type && curr->covers == covers) {
			if (rdataset != NULL)
				*rdataset = curr;
			return (DNS_R_SUCCESS);
		}
	}

	return (DNS_R_NOTFOUND);
}

/*
 * Read a name from buffer "source".
 */
static dns_result_t
getname(dns_name_t *name, isc_buffer_t *source, dns_message_t *msg,
	dns_decompress_t *dctx)
{
	isc_buffer_t *scratch;
	dns_result_t result;
	unsigned int tries;

	scratch = currentbuffer(msg);

	/*
	 * First try:  use current buffer.
	 * Second try:  allocate a new buffer and use that.
	 */
	tries = 0;
	while (tries < 2) {
		dns_name_init(name, NULL);
		result = dns_name_fromwire(name, source, dctx, ISC_FALSE,
					   scratch);

		if (result == DNS_R_NOSPACE) {
			tries++;

			result = newbuffer(msg, SCRATCHPAD_SIZE);
			if (result != DNS_R_SUCCESS)
				return (result);

			scratch = currentbuffer(msg);
		} else {
			return (result);
		}
	}

	INSIST(0);  /* Cannot get here... */
	return (DNS_R_UNEXPECTED);
}

static dns_result_t
getrdata(dns_name_t *name, isc_buffer_t *source, dns_message_t *msg,
	 dns_decompress_t *dctx, dns_rdataclass_t rdclass,
	 dns_rdatatype_t rdtype, unsigned int rdatalen, dns_rdata_t *rdata)
{
	isc_buffer_t *scratch;
	dns_result_t result;
	unsigned int tries;
	unsigned int trysize;

	/*
	 * In dynamic update messages, the rdata can be empty.
	 */
	if (msg->opcode == dns_opcode_update && rdatalen == 0) {
		/*
		 * When the rdata is empty, the data pointer is never
		 * dereferenced, but it must still be non-NULL.
		 */
		rdata->data = (unsigned char *)""; 
		rdata->length = 0;
		rdata->rdclass = rdclass;
		rdata->type = rdtype;
		return DNS_R_SUCCESS;
	}
	    
	scratch = currentbuffer(msg);

	isc_buffer_setactive(source, rdatalen);
	dns_decompress_localinit(dctx, name, source);

	/*
	 * First try:  use current buffer.
	 * Second try:  allocate a new buffer of size
	 *     max(SCRATCHPAD_SIZE, 2 * compressed_rdatalen)
	 *     (the data will fit if it was not more than 50% compressed)
	 * Subsequent tries: double buffer size on each try.
	 */
	tries = 0;
	trysize = 0;
	for (;;) {
		result = dns_rdata_fromwire(rdata, rdclass, rdtype,
					    source, dctx, ISC_FALSE,
					    scratch);

		if (result == DNS_R_NOSPACE) {
			if (tries == 0) {
				trysize = 2 * rdatalen;
				if (trysize < SCRATCHPAD_SIZE)
					trysize = SCRATCHPAD_SIZE;
			} else {
				INSIST(trysize != 0);
				if (trysize >= 65535)
					return (ISC_R_NOSPACE);
					/* XXX DNS_R_RRTOOLONG? */
				trysize *= 2;
			}
			tries++;
			result = newbuffer(msg, trysize);
			if (result != DNS_R_SUCCESS)
				return (result);

			scratch = currentbuffer(msg);
		} else {
			return (result);
		}
	}
}

static dns_result_t
getquestions(isc_buffer_t *source, dns_message_t *msg, dns_decompress_t *dctx)
{
	isc_region_t r;
	unsigned int count;
	dns_name_t *name;
	dns_name_t *name2;
	dns_rdataset_t *rdataset;
	dns_rdatalist_t *rdatalist;
	dns_result_t result;
	dns_rdatatype_t rdtype;
	dns_rdataclass_t rdclass;
	dns_namelist_t *section;

	section = &msg->sections[DNS_SECTION_QUESTION];

	for (count = 0 ; count < msg->counts[DNS_SECTION_QUESTION] ; count++) {
		name = newname(msg);
		if (name == NULL)
			return (DNS_R_NOMEMORY);

		/*
		 * Parse the name out of this packet.
		 */
		isc_buffer_remaining(source, &r);
		isc_buffer_setactive(source, r.length);
		result = getname(name, source, msg, dctx);
		if (result != DNS_R_SUCCESS)
			return (result);

		/*
		 * Run through the section, looking to see if this name
		 * is already there.  If it is found, put back the allocated
		 * name since we no longer need it, and set our name pointer
		 * to point to the name we found.
		 */
		result = findname(&name2, name, section);

		/*
		 * If it is the first name in the section, accept it.
		 *
		 * If it is not, but is not the same as the name already
		 * in the question section, append to the section.  Note that
		 * here in the question section this is illegal, so return
		 * FORMERR.  In the future, check the opcode to see if
		 * this should be legal or not.  In either case we no longer
		 * need this name pointer.
		 */
		if (result != DNS_R_SUCCESS) {
			if (ISC_LIST_EMPTY(*section)) {
				ISC_LIST_APPEND(*section, name, link);
			} else {
				return (DNS_R_FORMERR);
			}
		} else {
			name = name2;
		}

		/*
		 * Get type and class.
		 */
		isc_buffer_remaining(source, &r);
		if (r.length < 4)
			return (DNS_R_UNEXPECTEDEND);
		rdtype = isc_buffer_getuint16(source);
		rdclass = isc_buffer_getuint16(source);

		/*
		 * If this class is different than the one we already read,
		 * this is an error.
		 */
		if (msg->state == DNS_SECTION_ANY) {
			msg->state = DNS_SECTION_QUESTION;
			msg->rdclass = rdclass;
			msg->state = DNS_SECTION_QUESTION;
		} else if (msg->rdclass != rdclass)
			return (DNS_R_FORMERR);
		
		/*
		 * Can't ask the same question twice.
		 */
		result = findtype(NULL, name, rdtype, 0);
		if (result == DNS_R_SUCCESS)
			return (DNS_R_FORMERR);

		/*
		 * Allocate a new rdatalist.
		 */
		rdatalist = newrdatalist(msg);
		if (rdatalist == NULL)
			return (DNS_R_NOMEMORY);
		rdataset = newrdataset(msg);
		if (rdataset == NULL)
			return (DNS_R_NOMEMORY);

		/*
		 * Convert rdatalist to rdataset, and attach the latter to
		 * the name.
		 */
		rdatalist->type = rdtype;
		rdatalist->rdclass = rdclass;
		rdatalist->ttl = 0;
		ISC_LIST_INIT(rdatalist->rdata);

		dns_rdataset_init(rdataset);
		result = dns_rdatalist_tordataset(rdatalist, rdataset);
		if (result != DNS_R_SUCCESS)
			return (result);
		rdataset->attributes |= DNS_RDATASETATTR_QUESTION;

		ISC_LIST_APPEND(name->list, rdataset, link);
	}
	
	return (DNS_R_SUCCESS);
}

static dns_result_t
getsection(isc_buffer_t *source, dns_message_t *msg, dns_decompress_t *dctx,
	   dns_section_t sectionid, isc_boolean_t preserve_order)
{
	isc_region_t r;
	unsigned int count;
	unsigned int rdatalen;
	dns_name_t *name;
	dns_name_t *name2;
	dns_rdataset_t *rdataset;
	dns_rdatalist_t *rdatalist;
	dns_result_t result;
	dns_rdatatype_t rdtype, covers;
	dns_rdataclass_t rdclass;
	dns_rdata_t *rdata;
	dns_ttl_t ttl;
	dns_namelist_t *section;

	for (count = 0 ; count < msg->counts[sectionid] ; count++) {
		int recstart = source->current;
		isc_boolean_t skip_search;
		section = &msg->sections[sectionid];

		skip_search = ISC_FALSE;
		name = newname(msg);
		if (name == NULL)
			return (DNS_R_NOMEMORY);

		/*
		 * Parse the name out of this packet.
		 */
		isc_buffer_remaining(source, &r);
		isc_buffer_setactive(source, r.length);
		result = getname(name, source, msg, dctx);
		if (result != DNS_R_SUCCESS)
			return (result);

		/*
		 * Get type, class, ttl, and rdatalen.  Verify that at least
		 * rdatalen bytes remain.  (Some of this is deferred to
		 * later.)
		 */
		isc_buffer_remaining(source, &r);
		if (r.length < 2 + 2 + 4 + 2)
			return (DNS_R_UNEXPECTEDEND);
		rdtype = isc_buffer_getuint16(source);
		rdclass = isc_buffer_getuint16(source);

		/*
		 * If there was no question section, we may not yet have
		 * established a class.  Do so now.
		 */
		if (msg->state == DNS_SECTION_ANY) {
			if (rdclass == 0 || rdclass == dns_rdataclass_any)
				return (DNS_R_FORMERR);
			msg->rdclass = rdclass;
			msg->state = DNS_SECTION_QUESTION;
		}
		   
		/*
		 * If this class is different than the one in the question
		 * section, bail.
		 */
		if (msg->opcode != dns_opcode_update
		    && rdtype != dns_rdatatype_tsig
		    && rdtype != dns_rdatatype_opt
		    && msg->rdclass != rdclass)
			return (DNS_R_FORMERR);

		/*
		 * Special type handling for TSIG and OPT.
		 */
		if (rdtype == dns_rdatatype_tsig) {
			/*
			 * If it is a tsig, verify that it is in the
			 * additional data section, and switch sections for
			 * the rest of this rdata.
			 */
			if (sectionid != DNS_SECTION_ADDITIONAL)
				return (DNS_R_FORMERR);
			if (rdclass != dns_rdataclass_any)
				return (DNS_R_FORMERR);
			section = &msg->sections[DNS_SECTION_TSIG];
			msg->tsigstart = recstart;
			skip_search = ISC_TRUE;
		} else if (rdtype == dns_rdatatype_opt) {
			/*
			 * The name of an OPT record must be ".", it
			 * must be in the additional data section, and
			 * it must be the first OPT we've seen.
			 */
			if (!dns_name_equal(dns_rootname, name) ||
			    sectionid != DNS_SECTION_ADDITIONAL ||
			    msg->opt != NULL)
				return (DNS_R_FORMERR);
			skip_search = ISC_TRUE;
		}
		
		/*
		 * ... now get ttl and rdatalen, and check buffer.
		 */
		ttl = isc_buffer_getuint32(source);
		rdatalen = isc_buffer_getuint16(source);
		r.length -= (2 + 2 + 4 + 2);
		if (r.length < rdatalen)
			return (DNS_R_UNEXPECTEDEND);

		/*
		 * If we are doing a dynamic update don't bother searching
		 * for a name, just append this one to the end of the message.
		 */
		if (preserve_order || msg->opcode == dns_opcode_update ||
		    skip_search) {
			if (rdtype != dns_rdatatype_opt)
				ISC_LIST_APPEND(*section, name, link);
		} else {
			/*
			 * Run through the section, looking to see if this name
			 * is already there.  If it is found, put back the
			 * allocated name since we no longer need it, and set
			 * our name pointer to point to the name we found.
			 */
			result = findname(&name2, name, section);

			/*
			 * If it is a new name, append to the section.
			 */
			if (result == DNS_R_SUCCESS) {
				releasename(msg, name);
				name = name2;
			} else {
				ISC_LIST_APPEND(*section, name, link);
			}
		}

		/*
		 * Read the rdata from the wire format.  Interpret the 
		 * rdata according to its actual class, even if it had a
		 * DynDNS meta-class in the packet (unless this is a TSIG).
		 * Then put the meta-class back into the finished rdata.
		 */
		rdata = newrdata(msg);
		if (rdata == NULL)
			return (DNS_R_NOMEMORY);
		if (rdtype != dns_rdatatype_tsig)
			result = getrdata(name, source, msg, dctx,
					  msg->rdclass, rdtype,
					  rdatalen, rdata);
		else
			result = getrdata(name, source, msg, dctx,
					  rdclass, rdtype, rdatalen, rdata);
		if (result != DNS_R_SUCCESS)
			return (result);
		rdata->rdclass = rdclass;
		if (rdtype == dns_rdatatype_sig && rdata->length > 0)
			covers = dns_rdata_covers(rdata);
		else
			covers = 0;

		/*
		 * Search name for the particular type and class.
		 * Skip this stage if in update mode, or this is a TSIG.
		 */
		if (preserve_order || msg->opcode == dns_opcode_update ||
		    skip_search)
			result = DNS_R_NOTFOUND;
		else
			result = findtype(&rdataset, name, rdtype, covers);

		/*
		 * If we found an rdataset that matches, we need to
		 * append this rdata to that set.  If we did not, we need
		 * to create a new rdatalist, store the important bits there,
		 * convert it to an rdataset, and link the latter to the name.
		 * Yuck.
		 */
		if (result == DNS_R_NOTFOUND) {
			rdataset = newrdataset(msg);
			if (rdataset == NULL)
				return (DNS_R_NOMEMORY);
			rdatalist = newrdatalist(msg);
			if (rdatalist == NULL)
				return (DNS_R_NOMEMORY);

			rdatalist->type = rdtype;
			rdatalist->covers = covers;
			rdatalist->rdclass = rdclass;
			rdatalist->ttl = ttl;
			ISC_LIST_INIT(rdatalist->rdata);

			dns_rdataset_init(rdataset);
			dns_rdatalist_tordataset(rdatalist, rdataset);

			if (rdtype != dns_rdatatype_opt)
				ISC_LIST_APPEND(name->list, rdataset, link);
		}

		/*
		 * XXXRTH  Normalize TTLs.
		 */

		/*
		 * XXXMLG Perform a totally ugly hack here to pull
		 * the rdatalist out of the private field in the rdataset,
		 * and append this rdata to the rdatalist's linked list
		 * of rdata.
		 */
		rdatalist = (dns_rdatalist_t *)(rdataset->private1);

		ISC_LIST_APPEND(rdatalist->rdata, rdata, link);

		/*
		 * If this is an OPT record, remember it.  Also, set
		 * the extended rcode.
		 */
		if (rdtype == dns_rdatatype_opt) {
			unsigned int ercode;

			msg->opt = rdataset;
			ercode = (msg->opt->ttl & DNS_MESSAGE_EDNSRCODE_MASK)
				>> 20;
			msg->rcode |= ercode;
		}
	}
	
	return (DNS_R_SUCCESS);
}

dns_result_t
dns_message_parse(dns_message_t *msg, isc_buffer_t *source,
		  isc_boolean_t preserve_order)
{
	isc_region_t r;
	dns_decompress_t dctx;
	dns_result_t ret;
	isc_uint16_t tmpflags;

	REQUIRE(DNS_MESSAGE_VALID(msg));
	REQUIRE(source != NULL);
	REQUIRE(msg->from_to_wire == DNS_MESSAGE_INTENTPARSE);

	msg->header_ok = 0;
	msg->question_ok = 0;

	isc_buffer_remaining(source, &r);
	if (r.length < DNS_MESSAGE_HEADERLEN)
		return (DNS_R_UNEXPECTEDEND);

	msg->id = isc_buffer_getuint16(source);
	tmpflags = isc_buffer_getuint16(source);
	msg->opcode = ((tmpflags & DNS_MESSAGE_OPCODE_MASK)
		       >> DNS_MESSAGE_OPCODE_SHIFT);
	msg->rcode = (tmpflags & DNS_MESSAGE_RCODE_MASK);
	msg->flags = (tmpflags & DNS_MESSAGE_FLAG_MASK);
	msg->counts[DNS_SECTION_QUESTION] = isc_buffer_getuint16(source);
	msg->counts[DNS_SECTION_ANSWER] = isc_buffer_getuint16(source);
	msg->counts[DNS_SECTION_AUTHORITY] = isc_buffer_getuint16(source);
	msg->counts[DNS_SECTION_ADDITIONAL] = isc_buffer_getuint16(source);

	msg->header_ok = 1;

	/*
	 * -1 means no EDNS.
	 */
	dns_decompress_init(&dctx, -1, ISC_FALSE);

	if (dns_decompress_edns(&dctx) > 1 || !dns_decompress_strict(&dctx))
		dns_decompress_setmethods(&dctx, DNS_COMPRESS_GLOBAL);
	else
		dns_decompress_setmethods(&dctx, DNS_COMPRESS_GLOBAL14);


	ret = getquestions(source, msg, &dctx);
	if (ret != DNS_R_SUCCESS)
		return (ret);
	msg->question_ok = 1;

	ret = getsection(source, msg, &dctx, DNS_SECTION_ANSWER,
			 preserve_order);
	if (ret != DNS_R_SUCCESS)
		return (ret);

	ret = getsection(source, msg, &dctx, DNS_SECTION_AUTHORITY,
			 preserve_order);
	if (ret != DNS_R_SUCCESS)
		return (ret);

	ret = getsection(source, msg, &dctx, DNS_SECTION_ADDITIONAL,
			 preserve_order);
	if (ret != DNS_R_SUCCESS)
		return (ret);

	isc_buffer_remaining(source, &r);
	if (r.length != 0)
		return (DNS_R_FORMERR);

	if (!ISC_LIST_EMPTY(msg->sections[DNS_SECTION_TSIG])) {
		ret = dns_tsig_verify(source, msg);
		if (ret != DNS_R_SUCCESS)
			return ret;
	}

	return (DNS_R_SUCCESS);
}

dns_result_t
dns_message_renderbegin(dns_message_t *msg, isc_buffer_t *buffer)
{
	isc_region_t r;
	dns_result_t result;

	REQUIRE(DNS_MESSAGE_VALID(msg));
	REQUIRE(buffer != NULL);
	REQUIRE(msg->buffer == NULL);
	REQUIRE(msg->from_to_wire == DNS_MESSAGE_INTENTRENDER);

	/*
	 * Erase the contents of this buffer.
	 */
	isc_buffer_clear(buffer);

	/*
	 * Make certain there is enough for at least the header in this
	 * buffer.
	 */
	isc_buffer_available(buffer, &r);
	REQUIRE(r.length >= DNS_MESSAGE_HEADERLEN);

	result = dns_compress_init(&msg->cctx, -1, msg->mctx);
	if (result != DNS_R_SUCCESS)
		return (result);
	msg->need_cctx_cleanup = 1;

	/*
	 * Reserve enough space for the header in this buffer.
	 */
	isc_buffer_add(buffer, DNS_MESSAGE_HEADERLEN);

	msg->buffer = buffer;

	return (DNS_R_SUCCESS);
}

dns_result_t
dns_message_renderchangebuffer(dns_message_t *msg, isc_buffer_t *buffer)
{
	isc_region_t r, rn;

	REQUIRE(DNS_MESSAGE_VALID(msg));
	REQUIRE(buffer != NULL);
	REQUIRE(msg->buffer != NULL);

	/*
	 * ensure that the new buffer is empty, and has enough space to
	 * hold the current contents.
	 */
	isc_buffer_clear(buffer);

	isc_buffer_available(buffer, &rn);
	isc_buffer_used(msg->buffer, &r);
	REQUIRE(rn.length > r.length);

	/*
	 * Copy the contents from the old to the new buffer.
	 */
	isc_buffer_add(buffer, r.length);
	memcpy(rn.base, r.base, r.length);

	msg->buffer = buffer;

	return (DNS_R_SUCCESS);
}

dns_result_t
dns_message_renderrelease(dns_message_t *msg, unsigned int space)
{
	REQUIRE(DNS_MESSAGE_VALID(msg));
	REQUIRE(msg->buffer != NULL);

	if (msg->reserved < space)
		return (DNS_R_NOSPACE);

	msg->reserved -= space;

	return (DNS_R_SUCCESS);
}

dns_result_t
dns_message_renderreserve(dns_message_t *msg, unsigned int space)
{
	isc_region_t r;

	REQUIRE(DNS_MESSAGE_VALID(msg));
	REQUIRE(msg->buffer != NULL);

	isc_buffer_available(msg->buffer, &r);
	if (r.length < (space + msg->reserved))
		return (DNS_R_NOSPACE);

	msg->reserved += space;

	return (DNS_R_SUCCESS);
}

dns_result_t
dns_message_rendersection(dns_message_t *msg, dns_section_t sectionid,
			  unsigned int priority, unsigned int options)
{
	dns_namelist_t *section;
	dns_name_t *name, *next_name;
	dns_rdataset_t *rdataset, *next_rdataset;
	unsigned int count, total;
	dns_result_t result;
	isc_buffer_t st; /* for rollbacks */

	(void)priority; /* XXXMLG implement */
	(void)options;  /* XXXMLG implement */

	REQUIRE(DNS_MESSAGE_VALID(msg));
	REQUIRE(msg->buffer != NULL);
	REQUIRE(VALID_NAMED_SECTION(sectionid));

	section = &msg->sections[sectionid];

	name = ISC_LIST_HEAD(*section);
	if (name == NULL)
		return (DNS_R_SUCCESS);

	/*
	 * Shrink the space in the buffer by the reserved amount.
	 */
	msg->buffer->length -= msg->reserved;

	total = 0;
	while (name != NULL) {
		next_name = ISC_LIST_NEXT(name, link);

		rdataset = ISC_LIST_HEAD(name->list);
		while (rdataset != NULL) {
			next_rdataset = ISC_LIST_NEXT(rdataset, link);

			if (rdataset->attributes & DNS_RDATASETATTR_RENDERED) {
				rdataset = next_rdataset;
				continue;
			}

			st = *(msg->buffer);

			count = 0;
			result = dns_rdataset_towire(rdataset, name,
						     &msg->cctx,
						     msg->buffer, &count);

			total += count;

			/*
			 * If out of space, record stats on what we rendered
			 * so far, and return that status.
			 *
			 * XXXMLG Need to change this when
			 * dns_rdataset_towire() can render partial
			 * sets starting at some arbitary point in the set.
			 * This will include setting a bit in the
			 * rdataset to indicate that a partial rendering
			 * was done, and some state saved somewhere
			 * (probably in the message struct)
			 * to indicate where to continue from.
			 */
			if (result != DNS_R_SUCCESS) {
				dns_compress_rollback(&msg->cctx, st.used);
				*(msg->buffer) = st;  /* rollback */
				msg->buffer->length += msg->reserved;
				msg->counts[sectionid] += total;
				return (result);
			}

			rdataset->attributes |= DNS_RDATASETATTR_RENDERED;

			rdataset = next_rdataset;
		}

		name = next_name;
	}

	msg->buffer->length += msg->reserved;
	msg->counts[sectionid] += total;

	return (DNS_R_SUCCESS);
}

void
dns_message_renderheader(dns_message_t *msg, isc_buffer_t *target)
{
	isc_uint16_t tmp;
	isc_region_t r;

	REQUIRE(DNS_MESSAGE_VALID(msg));
	REQUIRE(target != NULL);

	isc_buffer_available(target, &r);
	REQUIRE(r.length >= DNS_MESSAGE_HEADERLEN);

	isc_buffer_putuint16(target, msg->id);

	tmp = ((msg->opcode << DNS_MESSAGE_OPCODE_SHIFT)
	       & DNS_MESSAGE_OPCODE_MASK);
	tmp |= (msg->rcode & DNS_MESSAGE_RCODE_MASK);
	tmp |= (msg->flags & DNS_MESSAGE_FLAG_MASK);

	isc_buffer_putuint16(target, tmp);
	isc_buffer_putuint16(target, msg->counts[DNS_SECTION_QUESTION]);
	isc_buffer_putuint16(target, msg->counts[DNS_SECTION_ANSWER]);
	isc_buffer_putuint16(target, msg->counts[DNS_SECTION_AUTHORITY]);
	tmp  = msg->counts[DNS_SECTION_ADDITIONAL]
		+ msg->counts[DNS_SECTION_TSIG];
	isc_buffer_putuint16(target, tmp);
}

dns_result_t
dns_message_renderend(dns_message_t *msg)
{
	isc_buffer_t tmpbuf;
	isc_region_t r;
	int result;
	dns_rdata_t rdata;
	unsigned int count;

	REQUIRE(DNS_MESSAGE_VALID(msg));
	REQUIRE(msg->buffer != NULL);

	if ((msg->rcode & ~DNS_MESSAGE_RCODE_MASK) != 0 && msg->opt == NULL) {
		/*
		 * We have an extended rcode but are not using EDNS.
		 */
		return (DNS_R_FORMERR);
	}

	/*
	 * If we've got an OPT record, render it.
	 */
	if (msg->opt != NULL) {
		result = dns_rdataset_first(msg->opt);
		if (result != ISC_R_SUCCESS)
			return (result);
		dns_rdataset_current(msg->opt, &rdata);
		result = dns_message_renderrelease(msg, 11 + rdata.length);
		if (result != ISC_R_SUCCESS)
			return (result);
		/*
		 * Set the extended rcode.
		 */
		msg->opt->ttl &= ~DNS_MESSAGE_EDNSRCODE_MASK;
		msg->opt->ttl |= ((msg->rcode << 20) &
				  DNS_MESSAGE_EDNSRCODE_MASK);
		/*
		 * Render.
		 */
		count = 0;
		result = dns_rdataset_towire(msg->opt, dns_rootname,
					     &msg->cctx, msg->buffer, &count);
		msg->counts[DNS_SECTION_ADDITIONAL] += count;
		if (result != ISC_R_SUCCESS)
			return (result);
	}

	if (msg->tsigkey != NULL ||
	    ((msg->flags & DNS_MESSAGEFLAG_QR) != 0 &&
	     msg->querytsigstatus != dns_rcode_noerror))
	{
		result = dns_tsig_sign(msg);
		if (result != DNS_R_SUCCESS)
			return (result);
		result = dns_message_rendersection(msg, DNS_SECTION_TSIG, 0,
						   0);
		if (result != DNS_R_SUCCESS)
			return (result);
	}

	isc_buffer_used(msg->buffer, &r);
	isc_buffer_init(&tmpbuf, r.base, r.length, ISC_BUFFERTYPE_BINARY);

	dns_message_renderheader(msg, &tmpbuf);

	msg->buffer = NULL;  /* forget about this buffer only on success XXX */

	dns_compress_invalidate(&msg->cctx);
	msg->need_cctx_cleanup = 0;

	return (DNS_R_SUCCESS);
}

dns_result_t
dns_message_firstname(dns_message_t *msg, dns_section_t section)
{
	REQUIRE(DNS_MESSAGE_VALID(msg));
	REQUIRE(VALID_NAMED_SECTION(section));

	msg->cursors[section] = ISC_LIST_HEAD(msg->sections[section]);

	if (msg->cursors[section] == NULL)
		return (DNS_R_NOMORE);

	return (DNS_R_SUCCESS);
}

dns_result_t
dns_message_nextname(dns_message_t *msg, dns_section_t section)
{
	REQUIRE(DNS_MESSAGE_VALID(msg));
	REQUIRE(VALID_NAMED_SECTION(section));
	REQUIRE(msg->cursors[section] != NULL);
	
	msg->cursors[section] = ISC_LIST_NEXT(msg->cursors[section], link);

	if (msg->cursors[section] == NULL)
		return (DNS_R_NOMORE);

	return (DNS_R_SUCCESS);
}

void
dns_message_currentname(dns_message_t *msg, dns_section_t section,
			dns_name_t **name)
{
	REQUIRE(DNS_MESSAGE_VALID(msg));
	REQUIRE(VALID_NAMED_SECTION(section));
	REQUIRE(name != NULL && *name == NULL);
	REQUIRE(msg->cursors[section] != NULL);

	*name = msg->cursors[section];
}

dns_result_t
dns_message_findname(dns_message_t *msg, dns_section_t section,
		     dns_name_t *target, dns_rdatatype_t type,
		     dns_rdatatype_t covers, dns_name_t **name,
		     dns_rdataset_t **rdataset)
{
	dns_name_t *foundname;
	dns_result_t result;

	/*
	 * XXX These requirements are probably too intensive, especially
	 * where things can be NULL, but as they are they ensure that if
	 * something is NON-NULL, indicating that the caller expects it
	 * to be filled in, that we can in fact fill it in.
	 */
	REQUIRE(msg != NULL);
	REQUIRE(VALID_SECTION(section));
	REQUIRE(target != NULL);
	if (name != NULL)
		REQUIRE(*name == NULL);
	if (type == dns_rdatatype_any) {
		REQUIRE(rdataset == NULL);
	} else {
		if (rdataset != NULL)
			REQUIRE(*rdataset == NULL);
	}

	/*
	 * Search through, looking for the name.
	 */
	result = findname(&foundname, target, &msg->sections[section]);
	if (result == DNS_R_NOTFOUND)
		return (DNS_R_NXDOMAIN);
	else if (result != DNS_R_SUCCESS)
		return (result);

	if (name != NULL)
		*name = foundname;

	/*
	 * And now look for the type.
	 */
	if (type == dns_rdatatype_any)
		return (DNS_R_SUCCESS);

	result = findtype(rdataset, foundname, type, covers);
	if (result == DNS_R_NOTFOUND)
		return (DNS_R_NXRDATASET);

	return (result);
}

void
dns_message_movename(dns_message_t *msg, dns_name_t *name,
		     dns_section_t fromsection,
		     dns_section_t tosection)
{
	REQUIRE(msg != NULL);
	REQUIRE(msg->from_to_wire == DNS_MESSAGE_INTENTRENDER);
	REQUIRE(name != NULL);
	REQUIRE(VALID_NAMED_SECTION(fromsection));
	REQUIRE(VALID_NAMED_SECTION(tosection));

	/*
	 * Unlink the name from the old section
	 */
	ISC_LIST_UNLINK(msg->sections[fromsection], name, link);
	ISC_LIST_APPEND(msg->sections[tosection], name, link);
}

void
dns_message_addname(dns_message_t *msg, dns_name_t *name,
		    dns_section_t section)
{
	REQUIRE(msg != NULL);
	REQUIRE(msg->from_to_wire == DNS_MESSAGE_INTENTRENDER);
	REQUIRE(name != NULL);
	REQUIRE(VALID_NAMED_SECTION(section));

	ISC_LIST_APPEND(msg->sections[section], name, link);
}

dns_result_t
dns_message_gettempname(dns_message_t *msg, dns_name_t **item)
{
	REQUIRE(DNS_MESSAGE_VALID(msg));
	REQUIRE(item != NULL && *item == NULL);

	*item = newname(msg);
	if (*item == NULL)
		return (DNS_R_NOMEMORY);

	return (DNS_R_SUCCESS);
}

dns_result_t
dns_message_gettemprdata(dns_message_t *msg, dns_rdata_t **item)
{
	REQUIRE(DNS_MESSAGE_VALID(msg));
	REQUIRE(item != NULL && *item == NULL);

	*item = newrdata(msg);
	if (*item == NULL)
		return (DNS_R_NOMEMORY);

	return (DNS_R_SUCCESS);
}

dns_result_t
dns_message_gettemprdataset(dns_message_t *msg, dns_rdataset_t **item)
{
	REQUIRE(DNS_MESSAGE_VALID(msg));
	REQUIRE(item != NULL && *item == NULL);

	*item = newrdataset(msg);
	if (*item == NULL)
		return (DNS_R_NOMEMORY);

	return (DNS_R_SUCCESS);
}

dns_result_t
dns_message_gettemprdatalist(dns_message_t *msg, dns_rdatalist_t **item)
{
	REQUIRE(DNS_MESSAGE_VALID(msg));
	REQUIRE(item != NULL && *item == NULL);

	*item = newrdatalist(msg);
	if (*item == NULL)
		return (DNS_R_NOMEMORY);

	return (DNS_R_SUCCESS);
}

void
dns_message_puttempname(dns_message_t *msg, dns_name_t **item)
{
	REQUIRE(DNS_MESSAGE_VALID(msg));
	REQUIRE(item != NULL && *item != NULL);

	releasename(msg, *item);
	*item = NULL;
}

void
dns_message_puttemprdata(dns_message_t *msg, dns_rdata_t **item)
{
	REQUIRE(DNS_MESSAGE_VALID(msg));
	REQUIRE(item != NULL && *item != NULL);

	releaserdata(msg, *item);
	*item = NULL;
}

void
dns_message_puttemprdataset(dns_message_t *msg, dns_rdataset_t **item)
{
	REQUIRE(DNS_MESSAGE_VALID(msg));
	REQUIRE(item != NULL && *item != NULL);

	releaserdataset(msg, *item);
	*item = NULL;
}

void
dns_message_puttemprdatalist(dns_message_t *msg, dns_rdatalist_t **item)
{
	REQUIRE(DNS_MESSAGE_VALID(msg));
	REQUIRE(item != NULL && *item != NULL);

	releaserdatalist(msg, *item);
	*item = NULL;
}

dns_result_t
dns_message_peekheader(isc_buffer_t *source, dns_messageid_t *idp,
		       unsigned int *flagsp)
{
	isc_region_t r;
	isc_buffer_t buffer;
	dns_messageid_t id;
	unsigned int flags;

	REQUIRE(source != NULL);

	buffer = *source;

	isc_buffer_remaining(&buffer, &r);
	if (r.length < DNS_MESSAGE_HEADERLEN)
		return (DNS_R_UNEXPECTEDEND);

	id = isc_buffer_getuint16(&buffer);
	flags = isc_buffer_getuint16(&buffer);
	flags &= DNS_MESSAGE_FLAG_MASK;

	if (flagsp != NULL)
		*flagsp = flags;
	if (idp != NULL)
		*idp = id;

	return (DNS_R_SUCCESS);
}

dns_result_t
dns_message_reply(dns_message_t *msg, isc_boolean_t want_question_section) {
	unsigned int first_section;

	REQUIRE(DNS_MESSAGE_VALID(msg));
	REQUIRE((msg->flags & DNS_MESSAGEFLAG_QR) == 0);

	if (!msg->header_ok)
		return (DNS_R_FORMERR);
	if (msg->opcode != dns_opcode_query)
		want_question_section = ISC_FALSE;
	if (want_question_section) {
		if (!msg->question_ok)
			return (DNS_R_FORMERR);
		first_section = DNS_SECTION_ANSWER;
	} else
		first_section = DNS_SECTION_QUESTION;
	msg->from_to_wire = DNS_MESSAGE_INTENTRENDER;
	msgresetnames(msg, first_section);
	msginitprivate(msg);
	/*
	 * We now clear most flags and then set QR, ensuring that the
	 * reply's flags will be in a reasonable state.
	 */
	msg->flags &= DNS_MESSAGE_REPLYPRESERVE;
	msg->flags |= DNS_MESSAGEFLAG_QR;

	/*
	 * This saves the query TSIG information for later use, if there is
	 * any.  This only happens once - that is, if dns_message_reply
	 * has already moved the variables, this has no effect.
	 */
	if (msg->tsig != NULL) {
		msg->querytsig = msg->tsig;
		msg->tsig = NULL;
		msg->querytsigstatus = msg->tsigstatus;
		msg->tsigstatus = dns_rcode_noerror;
	}

	return (DNS_R_SUCCESS);
}

dns_rdataset_t *
dns_message_getopt(dns_message_t *msg) {

	/*
	 * Get the OPT record for 'msg'.
	 */

	REQUIRE(DNS_MESSAGE_VALID(msg));
	
	return (msg->opt);
}

dns_result_t
dns_message_setopt(dns_message_t *msg, dns_rdataset_t *opt) {
	dns_result_t result;
	dns_rdata_t rdata;

	/*
	 * Set the OPT record for 'msg'.
	 */

	/*
	 * The space required for an OPT record is:
	 *
	 *	1 byte for the name
	 *	2 bytes for the type
	 *	2 bytes for the class
	 *	4 bytes for the ttl
	 *	2 bytes for the rdata length
	 * ---------------------------------
	 *     11 bytes
	 *
	 * plus the length of the rdata.
	 */

	REQUIRE(DNS_MESSAGE_VALID(msg));
	REQUIRE(opt->type == dns_rdatatype_opt);
	REQUIRE(msg->from_to_wire == DNS_MESSAGE_INTENTRENDER);
	REQUIRE(msg->buffer != NULL);
	REQUIRE(msg->state == DNS_SECTION_ANY);

	if (msg->opt != NULL) {
		result = dns_rdataset_first(msg->opt);
		if (result != ISC_R_SUCCESS)
			return (result);
		dns_rdataset_current(msg->opt, &rdata);
		result = dns_message_renderrelease(msg, 11 + rdata.length);
		dns_rdataset_disassociate(msg->opt);
		msg->opt = NULL;
	}

	result = dns_rdataset_first(opt);
	if (result != ISC_R_SUCCESS)
		return (result);
	dns_rdataset_current(opt, &rdata);
	result = dns_message_renderreserve(msg, 11 + rdata.length);
	if (result != ISC_R_SUCCESS)
		return (result);

	msg->opt = opt;

	return (DNS_R_SUCCESS);
}
