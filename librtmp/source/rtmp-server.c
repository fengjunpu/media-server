#include "rtmp-server.h"
#include "rtmp-internal.h"
#include "rtmp-msgtypeid.h"
#include "rtmp-handshake.h"
#include "rtmp-netstream.h"
#include "rtmp-netconnection.h"
#include "rtmp-control-message.h"
#include "rtmp-event.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>

#define RTMP_FMSVER			"FMS/3,0,1,123"
#define RTMP_CAPABILITIES	31

struct rtmp_server_t
{
	struct rtmp_t rtmp;

	void* param;
	struct rtmp_server_handler_t handler;

	uint8_t payload[2 * 1024];
	uint8_t handshake[2 * RTMP_HANDSHAKE_SIZE + 1]; // only for handshake
	size_t handshake_bytes;
	int handshake_state; // RTMP_HANDSHAKE_XXX

	char app[64]; // Server application name, e.g.: testapp
	char stream_name[256]; // Play/Publishing stream name, flv:sample, mp3:sample, H.264/AAC: mp4:sample.m4v
	char stream_type[18]; // Publishing type: live/record/append
	uint32_t stream_id; // createStream/deleteStream
	uint8_t receiveAudio; // 1-enable audio, 0-no audio
	uint8_t receiveVideo; // 1-enable video, 0-no video
};

static int rtmp_server_send_control(struct rtmp_t* rtmp, const uint8_t* payload, uint32_t bytes, uint32_t stream_id)
{
	struct rtmp_chunk_header_t header;
	header.fmt = RTMP_CHUNK_TYPE_0; // disable compact header
	header.cid = RTMP_CHANNEL_INVOKE;
	header.timestamp = 0;
	header.length = bytes;
	header.type = RTMP_TYPE_INVOKE;
	header.stream_id = stream_id; /* default 0 */
	return rtmp_chunk_write(rtmp, &header, payload);
}

static int rtmp_server_send_onstatus(struct rtmp_server_t* ctx, double transaction, int r, const char* success, const char* fail)
{
	r = rtmp_netstream_onstatus(ctx->payload, sizeof(ctx->payload), transaction, 0==r ? RTMP_LEVEL_STATUS : RTMP_LEVEL_ERROR, 0==r ? success : fail, "") - ctx->payload;
	return rtmp_server_send_control(&ctx->rtmp, ctx->payload, r, ctx->stream_id);
}

// handshake
static void rtmp_server_send_handshake(struct rtmp_server_t* ctx)
{
	int n, r;
	n = rtmp_handshake_s0(ctx->handshake, RTMP_VERSION);
	n += rtmp_handshake_s1(ctx->handshake + n, (uint32_t)time(NULL));
	n += rtmp_handshake_s2(ctx->handshake + n, (uint32_t)time(NULL), ctx->payload, RTMP_HANDSHAKE_SIZE);
	assert(n == 1 + RTMP_HANDSHAKE_SIZE + RTMP_HANDSHAKE_SIZE);
	r = ctx->handler.send(ctx->param, ctx->handshake, n);
	if (n != r)
		ctx->handler.onerror(ctx->param, -1, "error: send handshake");
}

/// 5.4.1. Set Chunk Size (1)
static void rtmp_server_send_set_chunk_size(struct rtmp_server_t* ctx)
{
	int r;
	r = rtmp_set_chunk_size(ctx->payload, sizeof(ctx->payload), ctx->rtmp.out_chunk_size);
	ctx->handler.send(ctx->param, ctx->payload, r);
}

/// 5.4.4. Window Acknowledgement Size (5)
static void rtmp_server_send_server_bandwidth(struct rtmp_server_t* ctx)
{
	int r;
	r = rtmp_window_acknowledgement_size(ctx->payload, sizeof(ctx->payload), ctx->rtmp.window_size);
	ctx->handler.send(ctx->param, ctx->payload, r);
}

/// 5.4.5. Set Peer Bandwidth (6)
static void rtmp_server_send_client_bandwidth(struct rtmp_server_t* ctx)
{
	int r;
	r = rtmp_set_peer_bandwidth(ctx->payload, sizeof(ctx->payload), ctx->rtmp.peer_bandwidth, RTMP_BANDWIDTH_LIMIT_DYNAMIC);
	ctx->handler.send(ctx->param, ctx->payload, r);
}

static void rtmp_server_send_stream_is_record(struct rtmp_server_t* ctx)
{
	int r;
	r = rtmp_event_stream_is_record(ctx->payload, sizeof(ctx->payload), ctx->stream_id);
	ctx->handler.send(ctx->param, ctx->payload, r);
}

static void rtmp_server_send_stream_begin(struct rtmp_server_t* ctx)
{
	int r;
	r = rtmp_event_stream_begin(ctx->payload, sizeof(ctx->payload), ctx->stream_id);
	ctx->handler.send(ctx->param, ctx->payload, r);
}

static void rtmp_server_onerror(void* param, int code, const char* msg)
{
	struct rtmp_server_t* ctx;
	ctx = (struct rtmp_server_t*)param;
	ctx->handler.onerror(ctx->param, code, msg);
}

static void rtmp_server_onabort(void* param, uint32_t chunk_stream_id)
{
	struct rtmp_server_t* ctx;
	ctx = (struct rtmp_server_t*)param;
	(void)chunk_stream_id;
	ctx->handler.onerror(ctx->param, -1, "client abort");
}

static void rtmp_server_onaudio(void* param, const uint8_t* data, size_t bytes, uint32_t timestamp)
{
	struct rtmp_server_t* ctx;
	ctx = (struct rtmp_server_t*)param;
	ctx->handler.onaudio(ctx->param, data, bytes, timestamp);
}

static void rtmp_server_onvideo(void* param, const uint8_t* data, size_t bytes, uint32_t timestamp)
{
	struct rtmp_server_t* ctx;
	ctx = (struct rtmp_server_t*)param;
	ctx->handler.onvideo(ctx->param, data, bytes, timestamp);
}

// 7.2.1.1. connect (p29)
// _result/_error
int rtmp_server_onconnect(void* param, int r, double transaction, const struct rtmp_connect_t* connect)
{
	struct rtmp_server_t* ctx;
	ctx = (struct rtmp_server_t*)param;

	if (0 == r)
	{
		assert(1 == (int)transaction);
		strlcpy(ctx->app, connect->app, sizeof(ctx->app));
		rtmp_server_send_server_bandwidth(ctx);
		rtmp_server_send_client_bandwidth(ctx);
		rtmp_server_send_set_chunk_size(ctx);

		r = rtmp_netconnection_connect_reply(ctx->payload, sizeof(ctx->payload), transaction, RTMP_FMSVER, RTMP_CAPABILITIES, "NetConnection.Connect.Success", RTMP_LEVEL_STATUS, "Connection Successed.") - ctx->payload;
		r = rtmp_server_send_control(&ctx->rtmp, ctx->payload, r, 0);
	}

	if (0 != r)
	{
		ctx->handler.onerror(ctx->param, r, "onconnect error");
	}
	return r;
}

// 7.2.1.3. createStream (p36)
// _result/_error
int rtmp_server_oncreate_stream(void* param, int r, double transaction)
{
	struct rtmp_server_t* ctx;
	ctx = (struct rtmp_server_t*)param;

	if (0 == r)
	{
		ctx->stream_id = 1;
		//r = ctx->handler.oncreate_stream(ctx->param, &ctx->stream_id);
		if (0 == r)
			r = rtmp_netconnection_create_stream_reply(ctx->payload, sizeof(ctx->payload), transaction, ctx->stream_id) - ctx->payload;
		else
			r = rtmp_netconnection_error(ctx->payload, sizeof(ctx->payload), transaction, "NetConnection.CreateStream.Failed", RTMP_LEVEL_ERROR, "createStream failed.") - ctx->payload;
		r = rtmp_server_send_control(&ctx->rtmp, ctx->payload, r, ctx->stream_id);
	}

	if (0 != r)
	{
		ctx->handler.onerror(ctx->param, r, "oncreate_stream error");
	}
	return r;
}

// 7.2.2.3. deleteStream (p43)
// The server does not send any response
int rtmp_server_ondelete_stream(void* param, int r, double transaction, double stream_id)
{
	struct rtmp_server_t* ctx;
	ctx = (struct rtmp_server_t*)param;

	if (0 == r)
	{
		stream_id = ctx->stream_id = 0; // clear stream id
		//r = ctx->handler.ondelete_stream(ctx->param, (uint32_t)stream_id);
		r = rtmp_server_send_onstatus(ctx, transaction, r, "NetStream.DeleteStream.Suceess", "NetStream.DeleteStream.Failed");
	}

	if (0 != r)
	{
		ctx->handler.onerror(ctx->param, r, "ondelete_stream error");
	}
	return r;
}

// 7.2.2.6. publish (p45)
// The server responds with the onStatus command
int rtmp_server_onpublish(void* param, int r, double transaction, const char* stream_name, const char* stream_type)
{
	struct rtmp_server_t* ctx;
	ctx = (struct rtmp_server_t*)param;

	if (0 == r)
	{
		ctx->handler.onpublish(ctx->param, ctx->app, stream_name, stream_type);
		if (0 == r)
		{
			strlcpy(ctx->stream_name, stream_name, sizeof(ctx->stream_name));
			strlcpy(ctx->stream_type, stream_type, sizeof(ctx->stream_type));

			// User Control (StreamBegin)
			rtmp_server_send_stream_begin(ctx);
		}

		r = rtmp_server_send_onstatus(ctx, transaction, r, "NetStream.DeleteStream.Suceess", "NetStream.DeleteStream.Failed");
	}

	if (0 != r)
	{
		ctx->handler.onerror(ctx->param, r, "onpublish error");
	}
	return r;
}

// 7.2.2.1. play (p38)
// reply onStatus NetStream.Play.Start & NetStream.Play.Reset
int rtmp_server_onplay(void* param, int r, double transaction, const char* stream_name, double start, double duration, uint8_t reset)
{
	struct rtmp_server_t* ctx;
	ctx = (struct rtmp_server_t*)param;

	if (0 == r)
	{
		r = ctx->handler.onplay(ctx->param, ctx->app, stream_name, start, duration, reset);
		if (0 == r)
		{
			strlcpy(ctx->stream_name, stream_name, sizeof(ctx->stream_name));
			strlcpy(ctx->stream_type, -1==start ? RTMP_STREAM_LIVE : RTMP_STREAM_RECORD, sizeof(ctx->stream_type));

			// SetChunkSize
			rtmp_server_send_set_chunk_size(ctx);
			// User Control (StreamIsRecorded)
			rtmp_server_send_stream_is_record(ctx);
			// User Control (StreamBegin)
			rtmp_server_send_stream_begin(ctx);

			// NetStream.Play.Reset
			if (reset) rtmp_server_send_onstatus(ctx, transaction, 0, "NetStream.Play.Reset", "NetStream.Play.Failed");
		}

		r = rtmp_server_send_onstatus(ctx, transaction, r, "NetStream.Play.Start", "NetStream.Play.Failed");
	}

	if (0 != r)
	{
		ctx->handler.onerror(ctx->param, r, "onplay error");
	}
	return r;
}

// 7.2.2.8. pause (p47)
// sucessful: NetStream.Pause.Notify/NetStream.Unpause.Notify
// failure: _error message
int rtmp_server_onpause(void* param, int r, double transaction, uint8_t pause, double milliSeconds)
{
	struct rtmp_server_t* ctx;
	ctx = (struct rtmp_server_t*)param;

	if (0 == r)
	{
		r = ctx->handler.onpause(ctx->param, pause, (uint32_t)milliSeconds);
		r = rtmp_server_send_onstatus(ctx, transaction, r, pause ? "NetStream.Pause.Notify" : "NetStream.Unpause.Notify", "NetStream.Pause.Failed");
	}

	if (0 != r)
	{
		ctx->handler.onerror(ctx->param, r, "onpause error");
	}
	return r;
}

// 7.2.2.7. seek (p46)
// successful : NetStream.Seek.Notify
// failure:  _error message
int rtmp_server_onseek(void* param, int r, double transaction, double milliSeconds)
{
	struct rtmp_server_t* ctx;
	ctx = (struct rtmp_server_t*)param;

	if (0 == r)
	{
		r = ctx->handler.onseek(ctx->param, (uint32_t)milliSeconds);
		r = rtmp_server_send_onstatus(ctx, transaction, r, "NetStream.Seek.Notify", "NetStream.Seek.Failed");
	}

	if (0 != r)
	{
		ctx->handler.onerror(ctx->param, r, "onseek error");
	}
	return r;
}

// 7.2.2.4. receiveAudio (p44)
// false: The server does not send any response,
// true: server responds with status messages NetStream.Seek.Notify and NetStream.Play.Start
int rtmp_server_onreceive_audio(void* param, int r, double transaction, uint8_t audio)
{
	struct rtmp_server_t* ctx;
	ctx = (struct rtmp_server_t*)param;

	if(0 == r)
	{
		ctx->receiveAudio = audio;
		if (audio)
		{
			r = rtmp_server_send_onstatus(ctx, transaction, r, "NetStream.Seek.Notify", "NetStream.Seek.Failed");
			r = rtmp_server_send_onstatus(ctx, transaction, r, "NetStream.Play.Start", "NetStream.Play.Failed");
		}
	}

	return r;
}

int rtmp_server_onreceive_video(void* param, int r, double transaction, uint8_t video)
{
	struct rtmp_server_t* ctx;
	ctx = (struct rtmp_server_t*)param;
	
	if(0 == r)
	{
		ctx->receiveVideo = video;
		if (video)
		{
			r = rtmp_server_send_onstatus(ctx, transaction, r, "NetStream.Seek.Notify", "NetStream.Seek.Failed");
			r = rtmp_server_send_onstatus(ctx, transaction, r, "NetStream.Play.Start", "NetStream.Play.Failed");
		}
	}

	return r;
}

static int rtmp_server_send(void* param, const uint8_t* header, uint32_t headerBytes, const uint8_t* payload, uint32_t payloadBytes)
{
	int r;
	struct rtmp_server_t* ctx;
	//static uint8_t s_payload[1024 * 2];
	ctx = (struct rtmp_server_t*)param;
	//memcpy(s_payload, header, headerBytes);
	//memcpy(s_payload + headerBytes, payload, payloadBytes);
	//r = ctx->handler.send(ctx->param, s_payload, headerBytes + payloadBytes);
	r = ctx->handler.send(ctx->param, header, headerBytes);
	if ((int)headerBytes == r && payloadBytes > 0)
		r = ctx->handler.send(ctx->param, payload, payloadBytes);
	return (r == (int)(payloadBytes > 0 ? payloadBytes : headerBytes)) ? 0 : -1;
}

void* rtmp_server_create(void* param, const struct rtmp_server_handler_t* handler)
{
	struct rtmp_server_t* ctx;
	ctx = (struct rtmp_server_t*)malloc(sizeof(*ctx));
	if (NULL == ctx)
		return NULL;

	memset(ctx, 0, sizeof(*ctx));
	memcpy(&ctx->handler, handler, sizeof(ctx->handler));
	ctx->param = param;
	ctx->stream_id = 0;
	ctx->receiveAudio = 1;
	ctx->receiveVideo = 1;
	ctx->handshake_state = RTMP_HANDSHAKE_UNINIT;

	ctx->rtmp.parser.state = RTMP_PARSE_INIT;
	ctx->rtmp.in_chunk_size = RTMP_CHUNK_SIZE;
	ctx->rtmp.out_chunk_size = RTMP_CHUNK_SIZE;
	ctx->rtmp.window_size = 2500000;
	ctx->rtmp.peer_bandwidth = 2500000;
	ctx->rtmp.buffer_length_ms = 30000;

	ctx->rtmp.param = ctx;
	ctx->rtmp.send = rtmp_server_send;
	ctx->rtmp.onaudio = rtmp_server_onaudio;
	ctx->rtmp.onvideo = rtmp_server_onvideo;
	ctx->rtmp.onerror = rtmp_server_onerror;
	ctx->rtmp.onabort = rtmp_server_onabort;
	ctx->rtmp.u.server.onconnect = rtmp_server_onconnect;
	ctx->rtmp.u.server.oncreate_stream = rtmp_server_oncreate_stream;
	ctx->rtmp.u.server.ondelete_stream = rtmp_server_ondelete_stream;
	ctx->rtmp.u.server.onpublish = rtmp_server_onpublish;
	ctx->rtmp.u.server.onplay = rtmp_server_onplay;
	ctx->rtmp.u.server.onpause = rtmp_server_onpause;
	ctx->rtmp.u.server.onseek = rtmp_server_onseek;
	ctx->rtmp.u.server.onreceive_audio = rtmp_server_onreceive_audio;
	ctx->rtmp.u.server.onreceive_video = rtmp_server_onreceive_video;
	
	return ctx;
}

void rtmp_server_destroy(void* rtmp)
{
	size_t i;
	struct rtmp_server_t* ctx;
	ctx = (struct rtmp_server_t*)rtmp;
	for (i = 0; i < N_CHUNK_STREAM; i++)
	{
		if (ctx->rtmp.in_packets->payload)
			free(ctx->rtmp.in_packets->payload);
	}

	free(ctx);
}

int rtmp_server_getstatus(void* p)
{
	struct rtmp_server_t* ctx;
	ctx = (struct rtmp_server_t*)p;
	return ctx->handshake_state;
}

int rtmp_server_input(void* rtmp, const uint8_t* data, size_t bytes)
{
	size_t n;
	const uint8_t* p;
	struct rtmp_server_t* ctx;
	ctx = (struct rtmp_server_t*)rtmp;

	p = data;
	while (bytes > 0)
	{
		switch (ctx->handshake_state)
		{
		case RTMP_HANDSHAKE_UNINIT: // C0: version
			ctx->handshake_state = RTMP_HANDSHAKE_0;
			ctx->handshake_bytes = 0; // clear buffer
			assert(*p <= RTMP_VERSION);
			bytes -= 1;
			p += 1;
			break;

		case RTMP_HANDSHAKE_0: // C1: 4-time + 4-zero + 1528-random
			assert(RTMP_HANDSHAKE_SIZE > ctx->handshake_bytes);
			n = RTMP_HANDSHAKE_SIZE - ctx->handshake_bytes;
			n = n <= bytes ? n : bytes;
			memcpy(ctx->payload + ctx->handshake_bytes, p, n);
			ctx->handshake_bytes += n;
			bytes -= n;
			p += n;

			if (ctx->handshake_bytes == RTMP_HANDSHAKE_SIZE)
			{
				ctx->handshake_state = RTMP_HANDSHAKE_1;
				ctx->handshake_bytes = 0; // clear buffer
				rtmp_server_send_handshake(ctx);
			}
			break;

		case RTMP_HANDSHAKE_1: // C2: 4-time + 4-time2 + 1528-echo
			assert(RTMP_HANDSHAKE_SIZE > ctx->handshake_bytes);
			n = RTMP_HANDSHAKE_SIZE - ctx->handshake_bytes;
			n = n <= bytes ? n : bytes;
			memcpy(ctx->payload + ctx->handshake_bytes, p, n);
			ctx->handshake_bytes += n;
			bytes -= n;
			p += n;

			if (ctx->handshake_bytes == RTMP_HANDSHAKE_SIZE)
			{
				ctx->handshake_state = RTMP_HANDSHAKE_2;
				ctx->handshake_bytes = 0; // clear buffer
			}
			break;

		case RTMP_HANDSHAKE_2:
		default:
			return rtmp_chunk_read(&ctx->rtmp, (const uint8_t*)data, bytes);
		}
	}

	return 0;
}

int rtmp_server_send_audio(void* rtmp, const void* data, size_t bytes, uint32_t timestamp)
{
	struct rtmp_server_t* ctx;
	struct rtmp_chunk_header_t header;
	ctx = (struct rtmp_server_t*)rtmp;
	if (0 == ctx->receiveAudio)
		return 0; // client don't want receive audio

	header.fmt = RTMP_CHUNK_TYPE_1; // enable compact header
	header.cid = RTMP_CHANNEL_AUDIO;
	header.timestamp = timestamp;
	header.length = (uint32_t)bytes;
	header.type = RTMP_TYPE_AUDIO;
	header.stream_id = ctx->stream_id;

	return rtmp_chunk_write(&ctx->rtmp, &header, (const uint8_t*)data);
}

int rtmp_server_send_video(void* rtmp, const void* data, size_t bytes, uint32_t timestamp)
{
	struct rtmp_server_t* ctx;
	struct rtmp_chunk_header_t header;
	ctx = (struct rtmp_server_t*)rtmp;
	if (0 == ctx->receiveVideo)
		return 0; // client don't want receive video

	header.fmt = RTMP_CHUNK_TYPE_1; // enable compact header
	header.cid = RTMP_CHANNEL_VIDEO;
	header.timestamp = timestamp;
	header.length = (uint32_t)bytes;
	header.type = RTMP_TYPE_VIDEO;
	header.stream_id = ctx->stream_id;

	return rtmp_chunk_write(&ctx->rtmp, &header, (const uint8_t*)data);
}

int rtmp_server_send_metadata(void* rtmp, const void* data, size_t bytes)
{
	struct rtmp_server_t* ctx;
	struct rtmp_chunk_header_t header;
	ctx = (struct rtmp_server_t*)rtmp;

	header.fmt = RTMP_CHUNK_TYPE_1; // enable compact header
	header.cid = RTMP_CHANNEL_INVOKE;
	header.timestamp = 0;
	header.length = (uint32_t)bytes;
	header.type = RTMP_TYPE_DATA;
	header.stream_id = ctx->stream_id;

	return rtmp_chunk_write(&ctx->rtmp, &header, (const uint8_t*)data);
}