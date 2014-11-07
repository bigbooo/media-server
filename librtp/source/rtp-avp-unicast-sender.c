#include "cstringext.h"
#include "rtp-avp-unicast-sender.h"
#include "sys/locker.h"
#include "sys/event.h"
#include <queue>

struct rtp_packet_header_t
{
	int32_t ref;
	size_t bytes;
};

typedef std::queue<struct rtp_packet_header_t*> packets_t;

struct rtp_unicast_sender_t
{
	char ip[32];
	u_short port[2]; // rtp/rtcp port
	aio_socket_t socket[2]; // rtp/rtcp socket
	byte_t data[MAX_UDP_BYTES]; // rtcp receive buffer

	void *members; // rtp source list
	void *senders; // rtp sender list
	struct rtp_source *source; // self info

	int32_t ref;
	struct rtp_packet_header_t *packet; // current send packet
	packets_t packets;
	locker_t locker;

	int code; // status code

	// statistics
	uint64_t bytes_failed;
	uint64_t packet_failed;
};

int rtp_avp_unicast_sender_init()
{
	return 0;
}

int rtp_avp_unicast_sender_cleanup()
{
	return 0;
}

static void rtp_avp_unicast_sender_release(struct rtp_unicast_sender_t *ctx)
{
	if(0 == atomic_decrement32(&ctx->ref))
	{
		if(ctx->socket[0])
			aio_socket_destroy(ctx->socket[0]);
		if(ctx->socket[1])
			aio_socket_destroy(ctx->socket[1]);
		if(ctx->members)
			rtp_source_list_destroy(ctx->members);
		if(ctx->senders)
			rtp_source_list_destroy(ctx->senders);
		if(ctx->source)
			rtp_source_release(ctx->source);

		locker_destroy(&ctx->locker);
		delete(ctx);
	}
}

void* rtp_avp_unicast_sender_create(const char* ip, u_short port[2], socket_t socket[2])
{
	struct rtp_unicast_sender_t *ctx;
	ctx = new struct rtp_unicast_sender_t();
	if(!ctx)
		return NULL;

	// set receiver socket buffer
	//socket_setrecvbuf(rtp, 50*1024);

	memset(ctx, 0, sizeof(*ctx));
	locker_create(&ctx->locker);
	ctx->source = rtp_source_create(ssrc);
	ctx->members = rtp_source_list_create();
	ctx->senders = rtp_source_list_create();
	ctx->socket[0] = aio_socket_create(socket[0], 1);
	ctx->socket[1] = aio_socket_create(socket[1], 1);
	if(!ctx->members || !ctx->senders || !ctx->source || !ctx->socket[0] || !ctx->socket[1])
	{
		rtp_avp_unicast_sender_destroy(ctx);
		return NULL;
	}

	strncpy(ctx->ip, ip, sizeof(ctx->ip));
	ctx->port[0] = port[0];
	ctx->port[1] = port[1];

	return ctx;
}

void rtp_avp_unicast_sender_destroy(void* sender)
{
	struct rtp_unicast_sender_t *ctx;
	ctx = (struct rtp_unicast_sender_t*)sender;
	rtp_avp_unicast_sender_release(ctx);
	return 0;
}

static int rtp_avp_unicast_sender_send_packet(struct rtp_unicast_sender_t *ctx)
{
	int r = 0;

	if(0 != ctx->code)
		return ctx->code;

	assert(!ctx->packet);
	assert(!ctx->packets.empty());
	ctx->packet = ctx->packets.front();

	atomic_increment32(&ctx->ref);
	r = aio_socket_sendto(ctx->socket[0], ctx->ip, ctx->port[0], ctx->packet+1, ctx->packet->bytes, rtp_avp_unicast_sender_onrtp, ctx);
	if(0 != r)
	{
		// statistics
		++ctx->packet_failed;
		ctx->bytes_failed += ctx->packet->bytes;

		ctx->packet = NULL;
		assert(ctx->ref > 1);
		atomic_decrement32(ctx);
	}

	return r;
}

static void rtp_avp_unicast_sender_onrtp(void* param, int code, size_t bytes)
{
	int r = 0;
	struct rtp_unicast_sender_t *ctx = NULL;
	struct rtp_packet_header_t *packet =  NULL;
	ctx = (struct rtp_unicast_sender_t *)param;

	assert(ctx->ref > 0);
	if(0 != code)
	{
		++ctx->packet_failed;
		ctx->bytes_failed += ctx->packet->bytes;
	}
	else
	{
		assert(bytes == ctx->packet->bytes);
	}

	locker_lock(&ctx->locker);
	packet = ctx->packet;
	ctx->packet = NULL;
	ctx->packets.pop();
	if(!ctx->packets.empty())
	{
		ctx->code = rtp_avp_unicast_sender_send_packet(ctx);
	}
	locker_unlock(&ctx->locker);

	if(0 == atomic_decrement32(&packet->ref))
	{
		free(packet);
	}

	rtp_avp_unicast_sender_release(ctx);
}

int rtp_avp_unicast_sender_send(void* sender, const void* data, size_t bytes)
{
	struct list_head *pos;
	struct rtp_packet_header_t *packet;
	struct rtp_unicast_sender_t *ctx;

	ctx = (struct rtp_unicast_sender_t*)sender;
	packet = (struct rtp_packet_header_t *)data - 1;

	assert(bytes == packet->bytes);
	atomic_increment32(&packet->ref);
	locker_lock(&ctx->locker);
	ctx->packets.push(packet);
	if(NULL == ctx->packet)
	{
		ctx->code = rtp_avp_unicast_sender_send_packet(ctx);
	}
	locker_unlock(&ctx->locker);
	return ctx->code;
}
