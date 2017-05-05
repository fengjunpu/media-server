#include "rtmp-internal.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

#define MIN(x, y) ((x) < (y) ? (x) : (y))

static struct rtmp_packet_t* rtmp_packet_find(struct rtmp_t* rtmp, uint32_t cid)
{
	uint32_t i;
	struct rtmp_packet_t* pkt;

	// The protocol supports up to 65597 streams with IDs 3-65599
	assert(cid <= 65535 + 64 && cid >= 2 /* Protocol Control Messages */);
	for (i = 0; i < N_CHUNK_STREAM; i++)
	{
		pkt = rtmp->in_packets + ((i + cid) % N_CHUNK_STREAM);
		if (pkt->header.cid == cid)
			return pkt;
	}
	return NULL;
}

static struct rtmp_packet_t* rtmp_packet_create(struct rtmp_t* rtmp, uint32_t cid)
{
	uint32_t i;
	struct rtmp_packet_t* pkt;

	// The protocol supports up to 65597 streams with IDs 3-65599
	assert(cid <= 65535 + 64 && cid >= 2 /* Protocol Control Messages */);
	assert(NULL == rtmp_packet_find(rtmp, cid));
	for (i = 0; i < N_CHUNK_STREAM; i++)
	{
		pkt = rtmp->in_packets + ((i + cid) % N_CHUNK_STREAM);
		if (0 == pkt->header.cid)
			return pkt;
	}
	return NULL;
}

static int rtmp_packet_alloc(struct rtmp_packet_t* packet, size_t bytes)
{
	if (packet->capacity < bytes)
	{
		void* p = realloc(packet->payload, bytes + 1024);
		if (!p)
			return ENOMEM;

		packet->payload = p;
		packet->capacity = bytes + 1024;
	}

	return 0;
}

static struct rtmp_packet_t* rtmp_packet_parse(struct rtmp_t* rtmp, const uint8_t* buffer)
{
	uint8_t fmt = 0;
	uint32_t cid = 0;
	struct rtmp_packet_t* packet;
	
	// chunk base header
	buffer += rtmp_chunk_basic_header_read(buffer, &fmt, &cid);

	// load previous header
	packet = rtmp_packet_find(rtmp, cid);
	if (NULL == packet)
	{
		if (RTMP_CHUNK_TYPE_0 != fmt && RTMP_CHUNK_TYPE_1 != fmt)
			return NULL; // don't know stream length

		packet = rtmp_packet_create(rtmp, cid);
		if (NULL == packet)
			return NULL;
	}

	// chunk message header
	packet->header.cid = cid;
	packet->header.fmt = fmt;
	rtmp_chunk_message_header_read(buffer, &packet->header);

	// alloc memory
	assert(packet->header.length > 0);
	if (0 != rtmp_packet_alloc(packet, packet->header.length))
		return NULL;

	return packet;
}

int rtmp_chunk_read(struct rtmp_t* rtmp, const uint8_t* data, size_t bytes)
{
	const static uint32_t s_header_size[] = { 11, 7, 3, 0 };

	size_t size, offset = 0;
	struct rtmp_parser_t* parser = &rtmp->parser;
	struct rtmp_chunk_header_t header;

	while (offset < bytes)
	{
		switch (parser->state)
		{
		case RTMP_PARSE_INIT:
			parser->pkt = NULL;
			parser->bytes = 1;
			parser->buffer[0] = data[offset++];

			if (0 == (parser->buffer[0] & 0x3F))
				parser->basic_bytes = 2;
			else if (1 == (parser->buffer[0] & 0x3F))
				parser->basic_bytes = 3;
			else
				parser->basic_bytes = 1;

			parser->state = RTMP_PARSE_BASIC_HEADER;
			break;

		case RTMP_PARSE_BASIC_HEADER:
			assert(parser->bytes <= parser->basic_bytes);
			while (parser->bytes < parser->basic_bytes && offset < bytes)
			{
				parser->buffer[parser->bytes++] = data[offset++];
			}

			assert(parser->bytes <= parser->basic_bytes);
			if (parser->bytes >= parser->basic_bytes)
			{
				parser->state = RTMP_PARSE_MESSAGE_HEADER;
			}
			break;

		case RTMP_PARSE_MESSAGE_HEADER:
			size = s_header_size[parser->buffer[0] >> 6] + parser->basic_bytes;
			assert(parser->bytes <= size);
			while (parser->bytes < size && offset < bytes)
			{
				parser->buffer[parser->bytes++] = data[offset++];
			}

			assert(parser->bytes <= size);
			if (parser->bytes >= size)
			{
				parser->state = RTMP_PARSE_EXTENDED_TIMESTAMP;
			}
			break;

		case RTMP_PARSE_EXTENDED_TIMESTAMP:
			assert(NULL == parser->pkt);
			parser->pkt = rtmp_packet_parse(rtmp, parser->buffer);
			if (NULL == parser->pkt) return ENOMEM;

			size = s_header_size[parser->pkt->header.fmt] + parser->basic_bytes;
			if (parser->pkt->header.timestamp >= 0xFFFFFF) size += 4;

			assert(parser->bytes <= size);
			while (parser->bytes < size && offset < bytes)
			{
				parser->buffer[parser->bytes++] = data[offset++];
			}

			assert(parser->bytes <= size);
			if (parser->bytes >= size)
			{
				// parse extended timestamp
				rtmp_chunk_extended_timestamp_read(parser->buffer + s_header_size[parser->buffer[0] >> 6] + parser->basic_bytes, &parser->pkt->header.timestamp);
				if(0 == parser->pkt->bytes) // first chunk
					parser->pkt->clock = (RTMP_CHUNK_TYPE_0 == parser->pkt->header.fmt) ? parser->pkt->header.timestamp : (parser->pkt->clock + parser->pkt->header.timestamp);
				parser->state = RTMP_PARSE_PAYLOAD;
			}
			break;

		case RTMP_PARSE_PAYLOAD:
			assert(parser->pkt);
			assert(parser->pkt->bytes < parser->pkt->header.length);
			assert(parser->pkt->capacity >= parser->pkt->header.length);
			size = MIN(rtmp->in_chunk_size - (parser->pkt->bytes % rtmp->in_chunk_size), parser->pkt->header.length - parser->pkt->bytes);
			size = MIN(size, bytes - offset);
			memcpy(parser->pkt->payload + parser->pkt->bytes, data + offset, size);
			parser->pkt->bytes += size;
			offset += size;

			if (parser->pkt->bytes >= parser->pkt->header.length)
			{
				assert(parser->pkt->bytes == parser->pkt->header.length);
				parser->state = RTMP_PARSE_INIT; // reset parser state
				parser->pkt->bytes = 0; // clear bytes

				memcpy(&header, &parser->pkt->header, sizeof(header));
				header.timestamp = parser->pkt->clock;
				rtmp_handler(rtmp, &header, parser->pkt->payload);
			}
			else if (0 == parser->pkt->bytes % rtmp->in_chunk_size)
			{
				// next chunk
				parser->state = RTMP_PARSE_INIT;
			}
			else
			{
				// need more data
				assert(offset == bytes);
			}
			break;

		default:
			assert(0);
			break;
		}
	}

	return 0;
}