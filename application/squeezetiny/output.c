/*
 *  Squeezelite - lightweight headless squeezebox emulator
 *
 *  (c) Adrian Smith 2012-2014, triode1@btinternet.com
 *	(c) Philippe 2015-2017, philippe_44@outlook.com
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "squeezelite.h"
#include "FLAC/stream_encoder.h"

extern log_level	output_loglevel;
static log_level 	*loglevel = &output_loglevel;

#define LOCK_O 	 mutex_lock(ctx->outputbuf->mutex)
#define UNLOCK_O mutex_unlock(ctx->outputbuf->mutex)

static size_t 	gain_and_fade(size_t frames, size_t min_frames, u8_t shift, struct thread_ctx_s *ctx);
static void 	lpcm_pack(u8_t *dst, u8_t *src, size_t bytes, u8_t channels, int endian);
static void apply_gain(s32_t *iptr, u32_t fade, u32_t gain, u8_t shift, size_t frames);
static void 	apply_cross(struct buffer *outputbuf, s32_t *cptr, u32_t fade,
							u32_t gain_in, u32_t gain_out, u8_t shift, size_t frames);
static void 	to_mono(s32_t *iptr,  size_t frames);
static void 	scale_and_pack(void *dst, u32_t *src, size_t frames, u8_t channels,
							   u8_t sample_size, int endian);

static FLAC__StreamEncoderWriteStatus flac_write_callback(const FLAC__StreamEncoder *encoder, const FLAC__byte buffer[], size_t bytes, unsigned samples, unsigned current_frame, void *client_data);

#if !LINKALL
static struct {
	// FLAC symbols to be dynamically loaded
	FLAC__StreamEncoder* (*FLAC__stream_encoder_new)(void);
	FLAC__bool (*FLAC__stream_encoder_finish)(FLAC__StreamEncoder *encoder);
	void (*FLAC__stream_encoder_delete)(FLAC__StreamEncoder *encoder);
	FLAC__bool (*FLAC__stream_encoder_set_verify)(FLAC__StreamEncoder*encoder, FLAC__bool value);
	FLAC__bool (*FLAC__stream_encoder_set_compression_level)(FLAC__StreamEncoder *encoder, unsigned value);
	FLAC__bool (*FLAC__stream_encoder_set_channels)(FLAC__StreamEncoder *encoder, unsigned value);
	FLAC__bool (*FLAC__stream_encoder_set_bits_per_sample)(FLAC__StreamEncoder *encoder, unsigned value);
	FLAC__bool (*FLAC__stream_encoder_set_sample_rate)(FLAC__StreamEncoder *encoder, unsigned value);
	FLAC__bool (*FLAC__stream_encoder_set_blocksize)(FLAC__StreamEncoder *encoder, unsigned value);
	FLAC__bool (*FLAC__stream_encoder_set_streamable_subset)(FLAC__StreamEncoder *encoder, FLAC__bool value);
	FLAC__StreamEncoderInitStatus (*FLAC__stream_encoder_init_stream)(FLAC__StreamEncoder *encoder, FLAC__StreamEncoderWriteCallback write_callback, FLAC__StreamEncoderSeekCallback seek_callback, FLAC__StreamEncoderTellCallback tell_callback, FLAC__StreamEncoderMetadataCallback metadata_callback, void *client_data);
	FLAC__bool (*FLAC__stream_encoder_process_interleaved)(FLAC__StreamEncoder *encoder, const FLAC__int32 buffer[], unsigned samples);
} f;
#endif

#define FLAC_BLOCK_SIZE 1024
#define FLAC_MIN_SPACE	(32*1024)

#define DRAIN_LEN		3
#define MAX_FRAMES_SEC 	10

#if LINKALL
#define FLAC(h, fn, ...) (FLAC__ ## fn)(__VA_ARGS__)
#define FLAC_A(h, a)     (FLAC__ ## a)
#else
#define FLAC(h, fn, ...) (h).FLAC__##fn(__VA_ARGS__)
#define FLAC_A(h, a)     (h).FLAC__ ## a
#endif


/*---------------------------------- WAVE ------------------------------------*/
static struct wave_header_s {
	u8_t 	chunk_id[4];
	u32_t	chunk_size;
	u8_t	format[4];
	u8_t	subchunk1_id[4];
	u8_t	subchunk1_size[4];
	u8_t	audio_format[2];
	u16_t	channels;
	u32_t	sample_rate;
	u32_t   byte_rate;
	u16_t	block_align;
	u16_t	bits_per_sample;
	u8_t	subchunk2_id[4];
	u32_t	subchunk2_size;
} wave_header = {
		{ 'R', 'I', 'F', 'F' },
		0,
		{ 'W', 'A', 'V', 'E' },
		{ 'f','m','t',' ' },
		{ 16, 0, 0, 0 },
		{ 1, 0 },
		0,
		0,
		0,
		0,
		0,
		{ 'd', 'a', 't', 'a' },
		0, 		//chunk_size - sizeof(struct wave_header_s) - 8 - 8
	};


/*---------------------------------- AIFF ------------------------------------*/
static struct aiff_header_s {			// need to use all u8 due to padding
	u8_t 	chunk_id[4];
	u8_t	chunk_size[4];
	u8_t	format[4];
	u8_t	common_id[4];
	u8_t	common_size[4];
	u8_t	channels[2];
	u8_t 	frames[4];
	u8_t	sample_size[2];
	u8_t	sample_rate_exp[2];
	u8_t	sample_rate_num[8];
	u8_t    data_id[4];
	u8_t	data_size[4];
	u32_t	offset;
	u32_t	blocksize;
} aiff_header = {
		{ 'F', 'O', 'R', 'M' },
#ifdef AIFF_MARKER
		{ 0x3B, 0x9A, 0xCA, 0x48 },		// adding comm, mark, ssnd and AIFF sizes
#else
		{ 0x3B, 0x9A, 0xCA, 0x2E },		// adding comm, ssnd and AIFF sizes
#endif
		{ 'A', 'I', 'F', 'F' },
		{ 'C', 'O', 'M', 'M' },
		{ 0x00, 0x00, 0x00, 0x12 },
		{ 0x00, 0x00 },
		{ 0x0E, 0xE6, 0xB2, 0x80 },		// 250x10^6 frames of 2 channels and 2 bytes each
		{ 0x00, 0x00 },
		{ 0x40, 0x0E },
		{ 0x00, 0x00 },
		{ 'S', 'S', 'N', 'D' },
		{ 0x3B, 0x9A, 0xCA, 0x08 },		// one chunk of 10^9 bytes + 8
	};


static void little16(void *dst, u16_t src);
static void little32(void *dst, u32_t src);
static void big16(void *dst, u16_t src);
static void big32(void *dst, u32_t src);


/*---------------------------------------------------------------------------*/
bool _output_fill(struct buffer *buf, struct thread_ctx_s *ctx) {
	size_t bytes = _buf_space(buf);
	struct outputstate *p = &ctx->output;

	/*
	The outputbuf contains FRAMES of 32 bits samples and 2 channels. The buf is
	what will be sent to client, so it can be anything amongst header, ICY data,
	passthru audio, uncompressed audio in 8,16,24,32 and 1 or 2 channels or
	re-compressed audio.
	The gain, truncation, L24 pack, swap, fade-in/out is applied *from* the
	outputbuf and copied *to* this buf so it really has no specific alignement
	but for simplicity we won't process anything until it has free space for the
	smallest block of audio which is BYTES_PER_FRAME
	Except for THRU mode, outputbuf->writep is always aligned to a multiple of
	BYTES_PER_FRAME when starting a	new track
	Output buffer (buf) cannot have an alignement due to additon of header  for
	wav and aif files
	*/
	if (bytes < BYTES_PER_FRAME) return true;

	// write header pending data and exit
	if (p->header.buffer) {
		bytes = min(p->header.count, _buf_cont_write(buf));
		memcpy(buf->writep, p->header.buffer + p->header.size - p->header.count, bytes);
		_buf_inc_writep(buf, bytes);
		p->header.count -= bytes;

		if (!p->header.count) {
			LOG_INFO("[%p] PCM header sent (%u bytes)", ctx, p->header.size);
			NFREE(p->header.buffer);
		}
		return true;
	}

	// might be overloaded if there is some ICY or header to be sent
	bytes = min(bytes, _buf_cont_read(ctx->outputbuf));

	// check if ICY sending is active
	if (p->icy.interval && !p->icy.count) {
		if (!p->icy.remain) {
			int len_16 = 0;

			LOG_SDEBUG("[%p]: ICY checking", ctx);

			if (p->icy.updated) {
				// there is room for 1 extra byte at the beginning for length
#if ICY_ARTWORK
				len_16 = sprintf(p->icy.buffer, "NStreamTitle='%s - %s';StreamURL='%s';",
								 p->icy.artist, p->icy.title, p->icy.artwork) - 1;
				LOG_INFO("[%p]: ICY update\n\t%s\n\t%s\n\t%s", ctx, p->icy.artist, p->icy.title, p->icy.artwork);
#else
				len_16 = sprintf(p->icy.buffer, "NStreamTitle='%s - %s'", p->icy.artist, p->icy.title) - 1;
				LOG_INFO("[%p]: ICY update\n\t%s\n\t%s", ctx, p->icy.artist, p->icy.title);
#endif
				len_16 = (len_16 + 15) / 16;
			}

			p->icy.buffer[0] = len_16;
			p->icy.size = p->icy.count = len_16 * 16 + 1;
			p->icy.remain = p->icy.interval;
			p->icy.updated = false;
		} else {
			// do not go over icy interval
			bytes = min(bytes, p->icy.remain);
		}
	}

	// write ICY pending data and exit
	if (p->icy.count) {
		bytes = min(p->icy.count, _buf_cont_write(buf));
		memcpy(buf->writep, p->icy.buffer + p->icy.size - p->icy.count, bytes);
		_buf_inc_writep(buf, bytes);
		p->icy.count -= bytes;
		return true;
	}

	// now proceeding audio data
	if (p->encode.mode == ENCODE_THRU) {
		//	simple encoded audio, nothing to process, just forward outputbuf
		bytes = min(bytes, _buf_cont_write(buf));
		memcpy(buf->writep, ctx->outputbuf->readp, bytes);
		_buf_inc_writep(buf, bytes);
		_buf_inc_readp(ctx->outputbuf, bytes);
		if (p->icy.interval) p->icy.remain -= bytes;
	} else {
		// uncompressed audio to be processed
		size_t in, out, frames = 0, bytes_per_frame = (p->encode.sample_size / 8) * p->encode.channels;
		u8_t *optr, obuf[BYTES_PER_FRAME*2];

		// outputbuf is processed by BYTES_PER_FRAMES multiples => aligns fine
		in = min(_buf_used(ctx->outputbuf), _buf_cont_read(ctx->outputbuf));

		if (p->encode.mode == ENCODE_PCM) {
			u8_t min_frames;

			// no bytes may mean end of audio data, let caller decide
			if (!in) return false;

			// in case of L24_LPCM, we need 2 frames at least
			if (p->encode.sample_size == 24 && ctx->config.L24_format == L24_PACKED_LPCM && p->format == 'p') min_frames = 2;
			else min_frames = 1;

			// buf cannot count on alignement because of headers (wav /aif)
			out = min(_buf_space(buf), _buf_cont_write(buf));

			// not enough cont'd place in output, just process one frame
			if (out < bytes_per_frame * min_frames) {
				optr = obuf;
				out = bytes_per_frame * min_frames;
			} else optr = buf->writep;

			frames = min(in / BYTES_PER_FRAME, out / bytes_per_frame);
			frames = min(frames, p->encode.sample_rate / MAX_FRAMES_SEC);

			// fading & gain
			frames = gain_and_fade(frames, min_frames, 0, ctx);

			// not able to process at that time
			if (!frames) return true;

			if (min_frames == 2)
				lpcm_pack((u8_t*) optr, ctx->outputbuf->readp, frames * BYTES_PER_FRAME,
						  p->encode.channels, 1);
			else
				scale_and_pack(optr, (u32_t*) ctx->outputbuf->readp, frames,
							   p->encode.channels, p->encode.sample_size, p->out_endian);

			// take the data from temporary buffer if needed
			if (optr == obuf) {
				size_t out = _buf_cont_write(buf);
				memcpy(buf->writep, optr, out);
				memcpy(buf->buf, optr + out, bytes_per_frame * min_frames - out);
			}

			_buf_inc_writep(buf, frames * bytes_per_frame);
		} else if (p->encode.mode == ENCODE_FLAC) {
			out = _buf_space(buf);

			// make sure FLAC has enough space to proceed
			if (out < FLAC_MIN_SPACE) return true;

			// no bytes may mean end of audio data, let caller decide
			if (!in) return false;

			frames = min(in / BYTES_PER_FRAME, out / bytes_per_frame);
			frames = min(frames, p->encode.sample_rate / MAX_FRAMES_SEC);

			// fading & gain
			frames = gain_and_fade(frames, 1, 32 - p->encode.sample_size, ctx);

			// not able to process at that time
			if (!frames) return true;

			if (p->encode.channels == 1) to_mono((s32_t*) ctx->outputbuf->readp, frames);
			FLAC(f, stream_encoder_process_interleaved, p->encode.codec, (FLAC__int32*) ctx->outputbuf->readp, frames);
		}

		_buf_inc_readp(ctx->outputbuf, frames * BYTES_PER_FRAME);

		LOG_SDEBUG("[%p]: processed %u frames", ctx, frames);
	}

	return (bytes != 0);
}

/*---------------------------------------------------------------------------*/
void _output_new_stream(struct buffer *obuf, struct thread_ctx_s *ctx) {
	struct outputstate *out = &ctx->output;

	if (!out->encode.sample_rate) out->encode.sample_rate = out->sample_rate;
	if (!out->encode.channels) out->encode.channels = out->channels;
	if (!out->encode.sample_size) {
		if (ctx->config.L24_format == L24_TRUNC16 && out->sample_size == 24) out->encode.sample_size = 16;
		else out->encode.sample_size = out->sample_size;
	}


	if (out->encode.mode == ENCODE_PCM) {
		size_t length;

		buf_init(obuf, out->encode.sample_rate * out->encode.channels * out->encode.sample_size / 8 * DRAIN_LEN);

		/*
		do not create a size (content-length) when we really don't know it but
		when we set a content-length (http_mode > 0) then at least the headers
		should be consistent
		*/
		if (!out->duration || out->encode.flow) {
			if (ctx->config.stream_length < 0) length = MAX_FILE_SIZE;
			else length = ctx->config.stream_length;
			length = (length / ((u64_t) out->encode.sample_rate * out->encode.channels * out->encode.sample_size /8)) *
					 (u64_t) out->encode.sample_rate * out->encode.channels * out->encode.sample_size / 8;
		} else length = (((u64_t) out->duration * out->encode.sample_rate) / 1000) * out->encode.channels * (out->encode.sample_size / 8);

		switch (out->format) {
		case 'w': {
			struct wave_header_s *h = malloc(sizeof(struct wave_header_s));

			memcpy(h, &wave_header, sizeof(struct wave_header_s));
			little16(&h->channels, out->encode.channels);
			little16(&h->bits_per_sample, out->encode.sample_size);
			little32(&h->sample_rate, out->encode.sample_rate);
			little32(&h->byte_rate, out->encode.sample_rate * out->encode.channels * (out->encode.sample_size / 8));
			little16(&h->block_align, out->encode.channels * (out->encode.sample_size / 8));
			little32(&h->subchunk2_size, length);
			little32(&h->chunk_size, 36 + length);
			length += 36 + 8;
			out->header.buffer = (u8_t*) h;
			out->header.size = out->header.count = sizeof(struct wave_header_s);
		   }
		   break;
	   case 'i': {
			struct aiff_header_s *h = malloc(sizeof(struct aiff_header_s));

			memcpy(h, &aiff_header, sizeof(struct aiff_header_s));
			big16(h->channels, out->encode.channels);
			big16(h->sample_size, out->encode.sample_size);
			big16(h->sample_rate_num, out->encode.sample_rate);
			big32(&h->data_size, length + 8);
			big32(&h->chunk_size, (length+8+8) + (18+8) + 4);
			big32(&h->frames, length / (out->encode.channels * (out->encode.sample_size / 8)));
			length += (8+8) + (18+8) + 4 + 8;
			out->header.buffer = (u8_t*) h;
			out->header.size = out->header.count = 54; // can't count on structure due to alignment
			}
			break;
		case 'p':
			default:
			out->header.buffer = NULL;
			out->header.size = out->header.count = 0;
			//FIXME: why setting length here at 0?
			// length = 0;
			break;
		}

		if (ctx->config.stream_length > 0) ctx->output.length = length;
		//FIXME ctx->output.length = length;
		LOG_INFO("[%p]: PCM estimated size %zu", ctx, length);
	} else if (out->encode.mode == ENCODE_FLAC) {
		FLAC__StreamEncoder *codec;
		size_t size;
		bool ok;

		size = max((out->encode.sample_rate * out->encode.channels * out->encode.sample_size / 8 * DRAIN_LEN) / 2, 2 * FLAC_MIN_SPACE);
		buf_init(obuf, size);

		codec = FLAC(f, stream_encoder_new);
		ok = FLAC(f, stream_encoder_set_verify,codec, false);
		ok &= FLAC(f, stream_encoder_set_compression_level, codec, out->encode.level);
		ok &= FLAC(f, stream_encoder_set_channels, codec, out->encode.channels);
		ok &= FLAC(f, stream_encoder_set_bits_per_sample, codec, out->encode.sample_size);
		ok &= FLAC(f, stream_encoder_set_sample_rate, codec, out->encode.sample_rate);
		ok &= FLAC(f, stream_encoder_set_blocksize, codec, FLAC_BLOCK_SIZE);
		ok &= FLAC(f, stream_encoder_set_streamable_subset, codec, true);
		ok &= !FLAC(f, stream_encoder_init_stream, codec, flac_write_callback, NULL, NULL, NULL, obuf);
		out->encode.codec = (void*) codec;

		LOG_INFO("[%p]: encoding using FLAC (%u)", ctx, ok);
	} else {
		buf_init(obuf, max((out->bitrate * DRAIN_LEN) / 8, 128*1024));
	}
}

/*---------------------------------------------------------------------------*/
void _output_end_stream(bool finish, struct thread_ctx_s *ctx) {
	if (ctx->output.encode.mode == ENCODE_FLAC && ctx->output.encode.codec) {
		// FLAC is a pain and requires a last encode call
		LOG_INFO("[%p]: finishing FLAC", ctx);
		if (finish) FLAC(f, stream_encoder_finish, ctx->output.encode.codec);
		FLAC(f, stream_encoder_delete, ctx->output.encode.codec);
		ctx->output.encode.codec = NULL;
	}
}

/*---------------------------------------------------------------------------*/
void output_flush(struct thread_ctx_s *ctx) {
	int i;

	LOCK_O;

	ctx->render.ms_played = 0;
	/*
	Don't know actually if it's stopped or not but it will be and that stop event
	does not matter as we are flushing the whole thing. But if we want the next
	playback to work, better force that status to RD_STOPPED
	*/
	if (ctx->output.state != OUTPUT_OFF) ctx->output.state = OUTPUT_STOPPED;

	for (i = 0; i < 2; i++) if (ctx->output_thread[i].running) {
		ctx->output_thread[i].running = false;
		UNLOCK_O;
		pthread_join(ctx->output_thread[i].thread, NULL);
		LOCK_O;
	}

	ctx->output.track_started = false;
	ctx->output.track_start = NULL;
	ctx->output.encode.flow = false;
	NFREE(ctx->output.header.buffer);
	output_free_icy(ctx);
	_output_end_stream(false, ctx);
	ctx->render.index = -1;
	_buf_resize(ctx->outputbuf, OUTPUTBUF_IDLE_SIZE);

	UNLOCK_O;


	LOG_DEBUG("[%p]: flush output buffer", ctx);
}

/*---------------------------------------------------------------------------*/
bool output_init(struct thread_ctx_s *ctx) {
	LOG_DEBUG("[%p] init output media renderer", ctx);

	if (ctx->config.outputbuf_size <= OUTPUTBUF_IDLE_SIZE) ctx->config.outputbuf_size = OUTPUTBUF_SIZE;
	else ctx->config.outputbuf_size = (ctx->config.outputbuf_size * BYTES_PER_FRAME) / BYTES_PER_FRAME;
	ctx->outputbuf = &ctx->__o_buf;
	buf_init(ctx->outputbuf, OUTPUTBUF_IDLE_SIZE);

	ctx->output.track_started = false;
	ctx->output.track_start = NULL;
	ctx->output.encode.flow = false;
	ctx->output.encode.codec = NULL;
	ctx->output_thread[0].running = ctx->output_thread[1].running = false;
	ctx->output_thread[0].http = ctx->output_thread[1].http = -1;
	ctx->output.icy.artist = ctx->output.icy.title = ctx->output.icy.artwork = NULL;
	ctx->render.index = -1;

#if !LINKALL
	// load share dlibrary and symbols if necessary
	if (!f.FLAC__stream_encoder_new) {
		void *handle = dlopen(LIBFLAC, RTLD_NOW);

		if (handle) {
			f.FLAC__stream_encoder_new = dlsym(handle, "FLAC__stream_encoder_new");
			f.FLAC__stream_encoder_finish = dlsym(handle, "FLAC__stream_encoder_finish");
			f.FLAC__stream_encoder_delete = dlsym(handle, "FLAC__stream_encoder_delete");
			f.FLAC__stream_encoder_set_verify = dlsym(handle, "FLAC__stream_encoder_set_verify");
			f.FLAC__stream_encoder_set_compression_level = dlsym(handle, "FLAC__stream_encoder_set_compression_level");
			f.FLAC__stream_encoder_set_channels = dlsym(handle, "FLAC__stream_encoder_set_channels");
			f.FLAC__stream_encoder_set_bits_per_sample = dlsym(handle, "FLAC__stream_encoder_set_bits_per_sample");
			f.FLAC__stream_encoder_set_sample_rate = dlsym(handle, "FLAC__stream_encoder_set_sample_rate");
			f.FLAC__stream_encoder_set_blocksize = dlsym(handle, "FLAC__stream_encoder_set_blocksize");
			f.FLAC__stream_encoder_set_streamable_subset = dlsym(handle, "FLAC__stream_encoder_set_streamable_subset");
			f.FLAC__stream_encoder_init_stream = dlsym(handle, "FLAC__stream_encoder_init_stream");
			f.FLAC__stream_encoder_process_interleaved = dlsym(handle, "FLAC__stream_encoder_process_interleaved");
		} else {
			LOG_INFO("loading flac: %s", dlerror());
		}
	}
#endif

	return true;
}

/*---------------------------------------------------------------------------*/
void output_close(struct thread_ctx_s *ctx) {
	LOG_INFO("[%p] close media renderer", ctx);
	buf_destroy(ctx->outputbuf);
}

/*---------------------------------------------------------------------------*/
void output_free_icy(struct thread_ctx_s *ctx) {
	NFREE(ctx->output.icy.artist);
	NFREE(ctx->output.icy.title);
	NFREE(ctx->output.icy.artwork);
}

/*---------------------------------------------------------------------------*/
void _checkduration(u32_t frames, struct thread_ctx_s *ctx) {
	u32_t duration;

	if (!ctx->output.encode.flow) return;

	duration = ((u64_t) frames * 1000 ) / ctx->output.direct_sample_rate;

	if (duration != ctx->render.duration) {
		LOG_INFO("[%p]: duration adjustement %u %u", ctx, duration, ctx->render.duration);
		if (abs((s32_t) (duration - ctx->render.duration)) > 1000) {
			LOG_WARN("[%p]: skipping too big duration gap %u %u", ctx, duration, ctx->render.duration);
		} else ctx->render.duration = duration;
	}
}

/*---------------------------------------------------------------------------*/
static FLAC__StreamEncoderWriteStatus flac_write_callback(const FLAC__StreamEncoder *encoder, const FLAC__byte buffer[], size_t bytes, unsigned samples, unsigned current_frame, void *client_data) {
	struct buffer *obuf = (struct buffer*) client_data;
	unsigned out = _buf_space(obuf);

	/*
	Seems that this callback can be called multiple time after a call to the flac
	encoder ... so it's difficult to assess the mimum size required
	*/
	if (out < bytes) {
		LOG_ERROR("[%p]: not enough space for FLAC buffer %u %u", obuf, out, bytes);
		return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
	}

	out = min(_buf_cont_write(obuf), bytes);
	memcpy(obuf->writep, buffer, out);
	memcpy(obuf->buf, buffer + out, bytes - out);
	_buf_inc_writep(obuf, bytes);

	return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
}

/*---------------------------------------------------------------------------*/
void lpcm_pack(u8_t *dst, u8_t *src, size_t bytes, u8_t channels, int endian) {
	size_t i;

#if !SL_LITTLE_ENDIAN
	endian = !endian;
#endif

	// bytes are always a multiple of 12 (and 6 ...)
	// 3 bytes with packing required, 2 channels
	if (channels == 2) {
		if (endian) for (i = 0; i < bytes; i += 16) {
			// L0T,L0M & R0T,R0M
			*dst++ = src[3]; *dst++ = src[2];
			*dst++ = src[7]; *dst++ = src[6];
			// L1T,L1M & R1T,R1M
			*dst++ = src[9]; *dst++ = src[8];
			*dst++ = src[15]; *dst++ = src[14];
			// L0B, R0B, L1B, R1B
			*dst++ = src[1]; *dst++ = src[5]; *dst++ = src[9]; *dst++ = src[13];
			src += 16;
		} else for (i = 0; i < bytes; i += 16) {
			// L0T,L0M & R0T,R0M
			*dst++ = src[0]; *dst++ = src[1];
			*dst++ = src[4]; *dst++ = src[5];
			// L1T,L1M & R1T,R1M
			*dst++ = src[8]; *dst++ = src[9];
			*dst++ = src[12]; *dst++ = src[13];
			// L0B, R0B, L1B, R1B
			*dst++ = src[2]; *dst++ = src[6]; *dst++ = src[10]; *dst++ = src[14];
			src += 16;
		}
		// after that R0T,R0M,L0T,L0M,R1T,R1M,L1T,L1M,R0B,L0B,R1B,L1B
	}

	// 3 bytes with packing required, 1 channel
	if (channels == 1) {
		if (endian) for (i = 0; i < bytes; i += 16) {
			// C0T,C0M,C1,C1M
			*dst++ = src[3]; *dst++ = src[2];
			*dst++ = src[7]; *dst++ = src[6];
			// C0B, C1B
			*dst++ = src[1]; *dst++ = src[5];
			src += 16;
		} else for (i = 0; i < bytes; i += 16) {
			// C0T,C0M,C1,C1M
			*dst++ = src[0]; *dst++ = src[1];
			*dst++ = src[4]; *dst++ = src[5];
			// C0B, C1B
			*dst++ = src[2]; *dst++ = src[6];
			src += 16;
		}
		// after that C0T,C0M,C1T,C1M,C0B,C1B
	}
}

/*---------------------------------------------------------------------------*/
void scale_and_pack(void *dst, u32_t *src, size_t frames, u8_t channels, u8_t sample_size, int endian) {
	size_t count = frames * channels;

	if (channels == 2) {
		if (sample_size == 8) {
			u8_t *optr = (u8_t*) dst;
			if (endian) while (count--) *optr++ = (*src++ >> 24) ^ 0x80;
			else while (count--) *optr++ = *src++ >> 24;
		} else if (sample_size == 16) {
			u16_t *optr = (u16_t*) dst;
			if (endian) while (count--) *optr++ = *src++ >> 16;
			else while (count--) {
				*optr++ = ((*src >> 24) & 0xff) | ((*src >> 8) & 0xff00);
				src++;
			}
		} else if (sample_size == 24) {
			u8_t *optr = (u8_t*) dst;
			if (endian) while (count--) {
				*optr++ = *src >> 8;
				*optr++ = *src >> 16;
				*optr++ = *src++ >> 24;
			} else while (count--) {
				*optr++ = *src >> 24;
				*optr++ = *src >> 16;
				*optr++ = *src++ >> 8;
			}
		} else if (sample_size == 32) {
			u32_t *optr = (u32_t*) dst;
			if (endian) memcpy(dst, src, count * 4);
			else while (count--) {
				*optr++ = ((*src >> 24) & 0xff)     | ((*src >> 8)  & 0xff00) |
						  ((*src << 8)  & 0xff0000) | ((*src << 24) & 0xff000000);
				src++;
			}
		}

	} else if (channels == 1) {
		if (sample_size == 8) {
			u8_t *optr = (u8_t*) dst;
			if (endian) while (count--) {
				*optr++ = (*src >> 24) ^ 0x80;
				src += 2;
			} else while (count--) {
				*optr++ = *src >> 24;
				src += 2;
			}
		} else if (sample_size == 16) {
			u16_t *optr = (u16_t*) dst;
			if (endian) while (count--) {
				*optr++ = *src >> 16;
				src += 2;
			}
			else while (count--) {
				*optr++ = ((*src >> 24) & 0xff) | ((*src >> 8) & 0xff00);
				src += 2;
			}
		} else if (sample_size == 24) {
			u8_t *optr = (u8_t*) dst;
			if (endian) while (count--) {
				*optr++ = *src >> 8;
				*optr++ = *src >> 16;
				*optr++ = *src >> 24;
				src += 2;
			} else while (count--) {
				*optr++ = *src >> 24;
				*optr++ = *src >> 16;
				*optr++ = *src++ >> 8;
				src += 2;
			}
		} else if (sample_size == 32) {
			u32_t *optr = (u32_t*) dst;
			if (endian) while (count--) {
				*optr++ = *src;
				src += 2;
			} else while (count--) {
				*optr++ = ((*src >> 24) & 0xff)     | ((*src >> 8)  & 0xff00) |
						  ((*src << 8)  & 0xff0000) | ((*src << 24) & 0xff000000);
				src += 2;
			}
		}
	}
}

/*---------------------------------------------------------------------------*/
static void to_mono(s32_t *iptr,  size_t frames) {
	s32_t *optr = iptr;

	while (frames--) {
		*optr++ = *iptr;
		iptr += 2;
  }
}

/*---------------------------------------------------------------------------*/
void _checkfade(bool start, struct thread_ctx_s *ctx) {
	frames_t bytes;

	LOG_INFO("[%p]: fade mode: %u duration: %u %s", ctx, ctx->output.fade_mode, ctx->output.fade_secs, start ? "track-start" : "track-end");

	// encode sample_rate might not be set yet (means == source sample_rate)
	if (ctx->output.encode.sample_rate) bytes = ctx->output.encode.sample_rate * BYTES_PER_FRAME * ctx->output.fade_secs;
	else bytes = ctx->output.sample_rate * BYTES_PER_FRAME * ctx->output.fade_secs;
	if (ctx->output.fade_mode == FADE_INOUT) {
		// must be aligned on a frame boundary otherwise output process locks
		bytes = ((bytes / 2) / BYTES_PER_FRAME) * BYTES_PER_FRAME;
	}

	if (start && (ctx->output.fade_mode == FADE_IN || (ctx->output.fade_mode == FADE_INOUT && _buf_used(ctx->outputbuf) == 0))) {
		bytes = min(bytes, ctx->outputbuf->size - BYTES_PER_FRAME); // shorter than full buffer otherwise start and end align
		LOG_INFO("[%p]: fade IN: %u frames", ctx, bytes / BYTES_PER_FRAME);
		ctx->output.fade = FADE_DUE;
		ctx->output.fade_dir = FADE_UP;
		ctx->output.fade_start = ctx->outputbuf->writep;
		ctx->output.fade_end = ctx->output.fade_start + bytes;
		if (ctx->output.fade_end >= ctx->outputbuf->wrap) {
			ctx->output.fade_end -= ctx->outputbuf->size;
		}
	}

	if (!start && (ctx->output.fade_mode == FADE_OUT || ctx->output.fade_mode == FADE_INOUT)) {
		if (!ctx->output.fade) {
			bytes = min(_buf_used(ctx->outputbuf), bytes);
			LOG_INFO("[%p]: fade %s: %u frames", ctx, ctx->output.fade_mode == FADE_INOUT ? "IN-OUT" : "OUT", bytes / BYTES_PER_FRAME);
			ctx->output.fade = FADE_DUE;
			ctx->output.fade_dir = FADE_DOWN;
			ctx->output.fade_start = ctx->outputbuf->writep - bytes;
			if (ctx->output.fade_start < ctx->outputbuf->buf) {
				ctx->output.fade_start += ctx->outputbuf->size;
			}
			ctx->output.fade_end = ctx->outputbuf->writep;
		} else {
			// optionally, writep should be memorized for when continuous streaming is available
			ctx->output.fade = FADE_PENDING;
			LOG_INFO("[%p]: fade OUT delayed as IN still processing", ctx);
		}
	}

	if (start && ctx->output.fade_mode == FADE_CROSSFADE) {
		// condition will always be false except in flow mode
		if (_buf_used(ctx->outputbuf) != 0) {
			bytes = min(bytes, _buf_used(ctx->outputbuf));
			// max of 90% of outputbuf as we consume additional buffer during crossfade
			bytes = min(bytes, (((9 * ctx->outputbuf->size) / 10) / BYTES_PER_FRAME) * BYTES_PER_FRAME);
			LOG_INFO("[%p]: CROSSFADE: %u frames", ctx, bytes / BYTES_PER_FRAME);
			ctx->output.fade = FADE_DUE;
			ctx->output.fade_dir = FADE_CROSS;
			ctx->output.fade_start = ctx->outputbuf->writep - bytes;
			if (ctx->output.fade_start < ctx->outputbuf->buf) {
				ctx->output.fade_start += ctx->outputbuf->size;
			}
			ctx->output.fade_end = ctx->outputbuf->writep;
		}
	}
}

/*---------------------------------------------------------------------------*/
size_t gain_and_fade(size_t frames, size_t min_frames, u8_t shift, struct thread_ctx_s *ctx) {
	struct outputstate *out = &ctx->output;
	u32_t fade = 65536;
	s32_t *cptr = NULL;

	// avoid LOG_ERROR message if no frame to process
	if (!frames) return frames;

	// need to align replay_gain change
	if (out->track_start) {
		if (out->track_start == ctx->outputbuf->readp) {
			LOG_INFO("[%p]: track start rate:%u gain:%u", ctx, out->encode.sample_rate, out->next_replay_gain);
			if (out->fade == FADE_INACTIVE || out->fade_mode != FADE_CROSSFADE) out->replay_gain = out->next_replay_gain;
			out->track_start = NULL;
		} else if (out->track_start > ctx->outputbuf->readp) {
			// reduce frames so we find the next track start at beginning of next chunk
			frames = min(frames, (out->track_start - ctx->outputbuf->readp) / BYTES_PER_FRAME);
		}
	}

	if (out->fade == FADE_DUE) {
		LOG_SDEBUG("[%p] fade check at %p (start %p)", ctx, ctx->outputbuf->readp, ctx->output.fade_start);
		if (out->fade_start == ctx->outputbuf->readp) {
			LOG_INFO("[%p]: fade start reached", ctx);
			out->fade = FADE_ACTIVE;
		} else if (out->fade_start > ctx->outputbuf->readp) {
			frames = min(frames, (out->fade_start - ctx->outputbuf->readp) / BYTES_PER_FRAME);
		}
	}

	if (out->fade == FADE_ACTIVE || out->fade == FADE_PENDING) {
		// find position within fade
		frames_t cur_f = ctx->outputbuf->readp >= out->fade_start ? (ctx->outputbuf->readp - out->fade_start) / BYTES_PER_FRAME :
			(ctx->outputbuf->readp + ctx->outputbuf->size - out->fade_start) / BYTES_PER_FRAME;
		frames_t dur_f = out->fade_end >= out->fade_start ? (out->fade_end - out->fade_start) / BYTES_PER_FRAME :
			(out->fade_end + ctx->outputbuf->size - out->fade_start) / BYTES_PER_FRAME;

		LOG_SDEBUG("[%p] fading at %p (c:%u d:%u)", ctx, ctx->outputbuf->readp, cur_f, dur_f);

		if (cur_f >= dur_f) {
			// only happens in flow mode as decode starts with empty outputbuf
			if (out->fade_mode == FADE_INOUT && out->fade_dir == FADE_DOWN) {
				LOG_INFO("[%p]: fade down complete, starting fade up", ctx);
				out->fade_dir = FADE_UP;
				out->fade_start = ctx->outputbuf->readp;
				out->fade_end = ctx->outputbuf->readp + dur_f * BYTES_PER_FRAME;
				if (out->fade_end >= ctx->outputbuf->wrap) out->fade_end -= ctx->outputbuf->size;
				cur_f = 0;
			} else if (out->fade_mode == FADE_CROSSFADE) {
				LOG_INFO("[%p]: crossfade complete", ctx);
				if (_buf_used(ctx->outputbuf) >= dur_f * BYTES_PER_FRAME) {
					_buf_inc_readp(ctx->outputbuf, dur_f * BYTES_PER_FRAME);
					// current track is shorter due to crossfade
					ctx->render.duration -= (dur_f * 1000) / out->encode.sample_rate;
					LOG_INFO("[%p]: skipped crossfaded start", ctx);
				} else {
					LOG_WARN("[%p]: unable to skip crossfaded start", ctx);
				}
				out->fade = FADE_INACTIVE;
				out->replay_gain = out->next_replay_gain;
			} else {
				LOG_INFO("[%p]: fade complete", ctx);
				if (out->fade == FADE_PENDING) _checkfade(false, ctx);
				else out->fade = FADE_INACTIVE;
			}
		}

		// if fade in progress set fade gain, ensure cont_frames reduced so we get to end of fade at start of chunk
		if (out->fade) {
			// don't overshoot fade end
			if (out->fade_end > ctx->outputbuf->readp)
				frames = min(frames, (out->fade_end - ctx->outputbuf->readp) / BYTES_PER_FRAME);

			if (out->fade_dir == FADE_UP || out->fade_dir == FADE_DOWN) {
				if (out->fade_dir == FADE_DOWN) cur_f = dur_f - cur_f;
				fade = ((u64_t) cur_f << 16) / dur_f;
			} else if (out->fade_dir == FADE_CROSS) {
				size_t in = _buf_used(ctx->outputbuf) / BYTES_PER_FRAME;
				// cross fade requires special treatment done below
				if (in > dur_f + frames) {
					fade  = ((u64_t) cur_f << 16) / dur_f;
					cptr = (s32_t *)(out->fade_end + cur_f * BYTES_PER_FRAME);
				} else {
					/*
					Wait for decode to fill buffer. If the track is too short,
					then we'll never get out of this, but then the output thread
					will end on no data when the decoder stops, so at least we
					arenot stuck
					*/
					frames = in > dur_f ? in - dur_f : 0;
				}
			}
		}

		LOG_ERROR("[%p]: fade gain %d", ctx, fade);
	}

	// lpcm_pack requires at least 2 frames or buffer too short for cross fade
	if ((frames || out->fade_dir == FADE_CROSS) && frames < min_frames) {
		LOG_INFO("[%p]: not enough frames to proceed", ctx);
		return 0;
	}

	// now can apply various gain & fading
	if (cptr) apply_cross(ctx->outputbuf, cptr, fade, out->replay_gain, out->next_replay_gain, shift, frames);
	else apply_gain((s32_t*) ctx->outputbuf->readp, fade, out->replay_gain, shift, frames);

	return frames;
}

#define MAX_VAL32 0x7fffffffffffLL
/*---------------------------------------------------------------------------*/
static void apply_gain(s32_t *iptr, u32_t fade, u32_t gain, u8_t shift, size_t frames) {
	size_t count = frames * 2;
	s64_t sample;

	gain = gain ? ((u64_t) gain * fade) >> 16 : fade;

	if (gain == 65536 && !shift) return;

	if (gain == 65536) {
		if (shift == 8) while (count--) {*iptr = *iptr >> 8; iptr++; }
		else if (shift == 16) while (count--) { *iptr = *iptr >> 16; iptr++; }
		else if (shift == 24) while (count--) { *iptr = *iptr >> 24; iptr++; }
	} else {
		if (!shift) while (count--) {
			sample = *iptr * (s64_t) gain;
			if (sample > MAX_VAL32) sample = MAX_VAL32;
			else if (sample < -MAX_VAL32) sample = -MAX_VAL32;
			*iptr++ = sample >> 16;
		} else if (shift == 8) while (count--) {
			sample = *iptr * (s64_t) gain;
			if (sample > MAX_VAL32) sample = MAX_VAL32;
			else if (sample < -MAX_VAL32) sample = -MAX_VAL32;
			*iptr++ = sample >> 24;
		} else if (shift == 16) while (count--) {
			sample = *iptr * (s64_t) gain;
			if (sample > MAX_VAL32) sample = MAX_VAL32;
			else if (sample < -MAX_VAL32) sample = -MAX_VAL32;
			*iptr++ = sample >> 32;
		} else if (shift == 24) while (count--) {
			sample = *iptr * (s64_t) gain;
			if (sample > MAX_VAL32) sample = MAX_VAL32;
			else if (sample < -MAX_VAL32) sample = -MAX_VAL32;
			*iptr++ = sample >> 40;
		}
	}
}

/*---------------------------------------------------------------------------*/
void apply_cross(struct buffer *outputbuf, s32_t *cptr, u32_t fade, u32_t gain_in, u32_t gain_out, u8_t shift, size_t frames) {
	s32_t *iptr = (s32_t *) outputbuf->readp;
	frames_t count = frames * 2;
	s64_t sample;

	if (!gain_in) gain_in = 65536L;
	if (!gain_out) gain_out = 65536L;

	while (count--) {
		if (cptr > (s32_t *) outputbuf->wrap) cptr -= outputbuf->size / BYTES_PER_FRAME * 2;
		sample = ((*iptr * (s64_t) gain_in) >> 16) * (65536L - fade) + ((*cptr++ * (s64_t) gain_out) >> 16) * fade;
		if (sample > MAX_VAL32) sample = MAX_VAL32;
		else if (sample < -MAX_VAL32) sample = -MAX_VAL32;
		*iptr++ = sample >> (16 + shift);
	}
}

/*---------------------------------------------------------------------------*/
static void little16(void *dst, u16_t src)
{
	u8_t *p = (u8_t*) dst;

	*p++ = (u8_t) (src);
	*p = (u8_t) (src >> 8);
}

/*---------------------------------------------------------------------------*/
static void little32(void *dst, u32_t src)
{
	u8_t *p = (u8_t*) dst;

	*p++ = (u8_t) (src);
	*p++ = (u8_t) (src >> 8);
	*p++ = (u8_t) (src >> 16);
	*p = (u8_t) (src >> 24);

}

/*---------------------------------------------------------------------------*/
static void big16(void *dst, u16_t src)
{
	u8_t *p = (u8_t*) dst;

	*p++ = (u8_t) (src >> 8);
	*p = (u8_t) (src);
}

/*---------------------------------------------------------------------------*/
static void big32(void *dst, u32_t src)
{
	u8_t *p = (u8_t*) dst;

	*p++ = (u8_t) (src >> 24);
	*p++ = (u8_t) (src >> 16);
	*p++ = (u8_t) (src >> 8);
	*p = (u8_t) (src);
}





