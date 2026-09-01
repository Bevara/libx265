/*
 *			GPAC - Multimedia Framework C SDK
 *
 *  This file is part of GPAC / libx265 HEVC encoder filter - talks
 *  directly to libx265's native API (x265_encoder_open/encode/close),
 *  mirroring this repo's libx264 filter (filters/libx264/enc_x264.c) but
 *  for HEVC. Input caps match the raw YUV420 (GF_PIXEL_YUV) output
 *  already produced by this repo's video decoders directly - no
 *  intermediate pixel format negotiation needed.
 */

#include <gpac/filters.h>
#include <gpac/constants.h>
#include <gpac/mpeg4_odf.h>
#include <gpac/bitstream.h>
#include <string.h>

#include <x265.h>

typedef struct
{
	/* opts */
	u32 bitrate;
	const char *preset;

	GF_FilterPid *ipid, *opid;
	u32 width, height, pixel_format, stride, stride_uv, nb_planes, uv_height;
	GF_Fraction fps;

	x265_encoder *encoder;
	x265_param *param;
	Bool in_fmt_negotiate;
	Bool header_sent;
} GF_X265EncCtx;

/* Parses just enough of an HEVC SPS RBSP (profile_tier_level, general_*
 * fields only - see ITU-T H.265 7.3.3) to fill the general profile/tier/
 * level fields of a GF_HEVCConfig param_array entry. sps_nal points at the
 * start of the NAL unit including its 2-byte HEVC NAL header. */
static void x265enc_parse_sps_ptl(const u8 *sps_nal, u32 sps_size, GF_HEVCConfig *cfg)
{
	GF_BitStream *bs;
	if (sps_size < 15)
		return;

	bs = gf_bs_new(sps_nal, sps_size, GF_BITSTREAM_READ);
	gf_bs_read_int(bs, 16); /* nal_unit_header */
	gf_bs_read_int(bs, 4);  /* sps_video_parameter_set_id */
	gf_bs_read_int(bs, 3);  /* sps_max_sub_layers_minus1 */
	gf_bs_read_int(bs, 1);  /* sps_temporal_id_nesting_flag */

	/* profile_tier_level(1, sps_max_sub_layers_minus1) - general profile only,
	 * no sub-layer profile/level (this filter never sets sps_max_sub_layers>0) */
	cfg->profile_space = gf_bs_read_int(bs, 2);
	cfg->tier_flag = gf_bs_read_int(bs, 1);
	cfg->profile_idc = gf_bs_read_int(bs, 5);
	cfg->general_profile_compatibility_flags = gf_bs_read_int(bs, 32);
	cfg->progressive_source_flag = gf_bs_read_int(bs, 1);
	cfg->interlaced_source_flag = gf_bs_read_int(bs, 1);
	cfg->non_packed_constraint_flag = gf_bs_read_int(bs, 1);
	cfg->frame_only_constraint_flag = gf_bs_read_int(bs, 1);
	cfg->constraint_indicator_flags = ((u64)gf_bs_read_int(bs, 32)) << 12;
	cfg->constraint_indicator_flags |= gf_bs_read_int(bs, 12);
	cfg->level_idc = gf_bs_read_int(bs, 8);

	gf_bs_del(bs);
}

static GF_Err x265enc_send_config(GF_X265EncCtx *ctx)
{
	x265_nal *nals;
	u32 nb_nals, i;
	int ret;
	GF_HEVCConfig *cfg;
	u8 *dsi;
	u32 dsi_size;
	GF_Err e;
	Bool sps_parsed = GF_FALSE;

	ret = x265_encoder_headers(ctx->encoder, &nals, &nb_nals);
	if (ret < 0)
	{
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[X265Enc] Failed to retrieve VPS/SPS/PPS headers\n"));
		return GF_IO_ERR;
	}

	cfg = gf_odf_hevc_cfg_new();
	cfg->configurationVersion = 1;
	cfg->nal_unit_size = 4;
	cfg->chromaFormat = 1; /* 4:2:0 */
	cfg->luma_bit_depth = 8;
	cfg->chroma_bit_depth = 8;
	cfg->numTemporalLayers = 1;
	cfg->temporalIdNested = 1;

	for (i = 0; i < nb_nals; i++)
	{
		x265_nal *nal = &nals[i];
		u32 nal_type = (nal->payload[0] >> 1) & 0x3F;
		GF_HEVCParamArray *ar = NULL;
		GF_AVCConfigSlot *slot;
		u32 j, count;

		if ((nal_type != GF_HEVC_NALU_VID_PARAM) && (nal_type != GF_HEVC_NALU_SEQ_PARAM) && (nal_type != GF_HEVC_NALU_PIC_PARAM))
			continue;

		if (nal_type == GF_HEVC_NALU_SEQ_PARAM && !sps_parsed)
		{
			x265enc_parse_sps_ptl(nal->payload, nal->sizeBytes, cfg);
			sps_parsed = GF_TRUE;
		}

		count = gf_list_count(cfg->param_array);
		for (j = 0; j < count; j++)
		{
			GF_HEVCParamArray *a = gf_list_get(cfg->param_array, j);
			if (a->type == nal_type)
			{
				ar = a;
				break;
			}
		}
		if (!ar)
		{
			GF_SAFEALLOC(ar, GF_HEVCParamArray);
			ar->nalus = gf_list_new();
			ar->type = nal_type;
			ar->array_completeness = 1;
			gf_list_add(cfg->param_array, ar);
		}

		slot = (GF_AVCConfigSlot *)gf_malloc(sizeof(GF_AVCConfigSlot));
		memset(slot, 0, sizeof(GF_AVCConfigSlot));
		slot->size = (u16)nal->sizeBytes;
		slot->data = (u8 *)gf_malloc(nal->sizeBytes);
		memcpy(slot->data, nal->payload, nal->sizeBytes);
		gf_list_add(ar->nalus, slot);
	}

	dsi = NULL;
	dsi_size = 0;
	e = gf_odf_hevc_cfg_write(cfg, &dsi, &dsi_size);
	gf_odf_hevc_cfg_del(cfg);
	if (e != GF_OK)
	{
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[X265Enc] Failed to write hvcC config\n"));
		return e;
	}

	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_DECODER_CONFIG, &PROP_DATA_NO_COPY(dsi, dsi_size));

	ctx->header_sent = GF_TRUE;
	return GF_OK;
}

static GF_Err x265enc_setup(GF_Filter *filter, GF_X265EncCtx *ctx)
{
	if (ctx->encoder)
	{
		x265_encoder_close(ctx->encoder);
		ctx->encoder = NULL;
	}
	if (ctx->param)
	{
		x265_param_free(ctx->param);
		ctx->param = NULL;
	}

	ctx->param = x265_param_alloc();
	if (x265_param_default_preset(ctx->param, ctx->preset, "zerolatency") < 0)
	{
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[X265Enc] Unknown preset \"%s\", falling back to veryfast\n", ctx->preset));
		x265_param_default_preset(ctx->param, "veryfast", "zerolatency");
	}

	ctx->param->sourceWidth = ctx->width;
	ctx->param->sourceHeight = ctx->height;
	ctx->param->fpsNum = ctx->fps.num > 0 ? (u32)ctx->fps.num : 25;
	ctx->param->fpsDenom = ctx->fps.den ? ctx->fps.den : 1;
	ctx->param->internalCsp = X265_CSP_I420;

	/* force single-threaded: this WASM build links pthread symbols (x265's
	 * CMakeLists.txt unconditionally appends it on UNIX-like systems, which
	 * Emscripten's toolchain reports as) but this project's solver targets
	 * are not linked with -pthread/-sUSE_PTHREADS, so any thread pool x265
	 * tries to spin up per frameNumThreads/numaPools auto-detection can
	 * hang forever waiting on a thread that never actually schedules -
	 * same class of issue libx264 avoids via its --disable-thread configure
	 * flag. */
	ctx->param->frameNumThreads = 1;
	ctx->param->lookaheadThreads = 1;
	ctx->param->bEnableWavefront = 0;
	strncpy(ctx->param->numaPools, "none", X265_MAX_STRING_SIZE - 1);

	ctx->param->rc.rateControlMode = X265_RC_ABR;
	ctx->param->rc.bitrate = (int)ctx->bitrate;

	/* no B-frames: keeps decoding order == display order, avoiding the
	 * need to reorder output packets */
	ctx->param->bframes = 0;

	/* length-prefixed NALs (4-byte size), not Annex-B startcodes: this is
	 * the format isobmff_1 (and any GF_CODECID_HEVC consumer) expects for
	 * muxing, matching the hvcC config built in x265enc_send_config */
	ctx->param->bAnnexB = 0;
	ctx->param->bRepeatHeaders = 0;
	ctx->param->logLevel = X265_LOG_NONE;

	ctx->encoder = x265_encoder_open(ctx->param);
	if (!ctx->encoder)
	{
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[X265Enc] Failed to open encoder\n"));
		return GF_IO_ERR;
	}

	ctx->header_sent = GF_FALSE;
	return x265enc_send_config(ctx);
}

static GF_Err x265enc_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	const GF_PropertyValue *prop;
	GF_X265EncCtx *ctx = (GF_X265EncCtx *)gf_filter_get_udta(filter);

	if (is_remove)
	{
		if (ctx->opid)
		{
			gf_filter_pid_remove(ctx->opid);
			ctx->opid = NULL;
		}
		if (ctx->encoder)
		{
			x265_encoder_close(ctx->encoder);
			ctx->encoder = NULL;
		}
		if (ctx->param)
		{
			x265_param_free(ctx->param);
			ctx->param = NULL;
		}
		ctx->ipid = NULL;
		return GF_OK;
	}
	if (!gf_filter_pid_check_caps(pid))
		return GF_NOT_SUPPORTED;

	ctx->ipid = pid;

	if (!ctx->opid)
	{
		ctx->opid = gf_filter_pid_new(filter);
	}
	/* copy properties at init or reconfig */
	gf_filter_pid_copy_properties(ctx->opid, ctx->ipid);
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CODECID, &PROP_UINT(GF_CODECID_HEVC));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STRIDE, NULL);
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STRIDE_UV, NULL);
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_UNFRAMED, NULL);

	gf_filter_set_name(filter, "encx265:libx265");

	prop = gf_filter_pid_get_property(pid, GF_PROP_PID_WIDTH);
	if (!prop)
		return GF_OK;
	ctx->width = prop->value.uint;

	prop = gf_filter_pid_get_property(pid, GF_PROP_PID_HEIGHT);
	if (!prop)
		return GF_OK;
	ctx->height = prop->value.uint;

	prop = gf_filter_pid_get_property(pid, GF_PROP_PID_PIXFMT);
	if (!prop)
		return GF_OK;
	ctx->pixel_format = prop->value.uint;

	if (ctx->pixel_format != GF_PIXEL_YUV)
	{
		gf_filter_pid_negotiate_property(pid, GF_PROP_PID_PIXFMT, &PROP_UINT(GF_PIXEL_YUV));
		ctx->in_fmt_negotiate = GF_TRUE;
		return GF_OK;
	}
	ctx->in_fmt_negotiate = GF_FALSE;

	prop = gf_filter_pid_get_property(pid, GF_PROP_PID_FPS);
	if (prop)
		ctx->fps = prop->value.frac;
	else
	{
		ctx->fps.num = 25;
		ctx->fps.den = 1;
	}

	gf_pixel_get_size_info(ctx->pixel_format, ctx->width, ctx->height, NULL, &ctx->stride, &ctx->stride_uv, &ctx->nb_planes, &ctx->uv_height);

	return x265enc_setup(filter, ctx);
}

static GF_Err x265enc_send_nals(GF_X265EncCtx *ctx, x265_nal *nals, u32 nb_nals, x265_picture *pic_out)
{
	u32 i, total_size = 0;
	u8 *output;
	GF_FilterPacket *dst_pck;

	if (!nb_nals)
		return GF_OK;

	for (i = 0; i < nb_nals; i++)
		total_size += nals[i].sizeBytes;

	dst_pck = gf_filter_pck_new_alloc(ctx->opid, total_size, &output);
	if (!dst_pck)
		return GF_OUT_OF_MEM;

	total_size = 0;
	for (i = 0; i < nb_nals; i++)
	{
		memcpy(output + total_size, nals[i].payload, nals[i].sizeBytes);
		total_size += nals[i].sizeBytes;
	}

	gf_filter_pck_set_cts(dst_pck, (u64)pic_out->pts);
	gf_filter_pck_set_dts(dst_pck, (u64)pic_out->dts);
	gf_filter_pck_set_sap(dst_pck, (pic_out->sliceType == X265_TYPE_IDR) ? GF_FILTER_SAP_1 : GF_FILTER_SAP_NONE);

	gf_filter_pck_send(dst_pck);
	return GF_OK;
}

static GF_Err x265enc_process(GF_Filter *filter)
{
	GF_X265EncCtx *ctx = (GF_X265EncCtx *)gf_filter_get_udta(filter);
	GF_FilterPacket *pck;
	x265_picture *pic_in, *pic_out;
	x265_nal *nals;
	u32 nb_nals;
	int ret;
	const u8 *in_data;
	u32 size;

	if (ctx->in_fmt_negotiate)
		return GF_OK;

	pck = gf_filter_pid_get_packet(ctx->ipid);
	if (!pck)
	{
		if (gf_filter_pid_is_eos(ctx->ipid))
		{
			/* drain delayed frames before signaling EOS */
			pic_out = x265_picture_alloc();
			x265_picture_init(ctx->param, pic_out);
			while (ctx->encoder)
			{
				ret = x265_encoder_encode(ctx->encoder, &nals, &nb_nals, NULL, pic_out);
				if (ret <= 0)
					break;
				x265enc_send_nals(ctx, nals, nb_nals, pic_out);
			}
			x265_picture_free(pic_out);
			gf_filter_pid_set_eos(ctx->opid);
			return GF_EOS;
		}
		return GF_OK;
	}

	if (!ctx->encoder)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_SERVICE_ERROR;
	}

	in_data = (const u8 *)gf_filter_pck_get_data(pck, &size);
	if (!in_data)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_IO_ERR;
	}

	pic_in = x265_picture_alloc();
	x265_picture_init(ctx->param, pic_in);
	pic_in->colorSpace = X265_CSP_I420;
	pic_in->bitDepth = 8;

	pic_in->planes[0] = (void *)in_data;
	pic_in->stride[0] = (int)ctx->stride;
	pic_in->planes[1] = (void *)(in_data + ctx->stride * ctx->height);
	pic_in->stride[1] = (int)ctx->stride_uv;
	pic_in->planes[2] = (u8 *)pic_in->planes[1] + ctx->stride_uv * ctx->uv_height;
	pic_in->stride[2] = (int)ctx->stride_uv;

	pic_in->pts = (int64_t)gf_filter_pck_get_cts(pck);

	pic_out = x265_picture_alloc();
	x265_picture_init(ctx->param, pic_out);

	ret = x265_encoder_encode(ctx->encoder, &nals, &nb_nals, pic_in, pic_out);
	gf_filter_pid_drop_packet(ctx->ipid);
	x265_picture_free(pic_in);

	if (ret < 0)
	{
		x265_picture_free(pic_out);
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[X265Enc] Encoding failed\n"));
		return GF_NON_COMPLIANT_BITSTREAM;
	}

	x265enc_send_nals(ctx, nals, nb_nals, pic_out);
	x265_picture_free(pic_out);
	return GF_OK;
}

static void x265enc_finalize(GF_Filter *filter)
{
	GF_X265EncCtx *ctx = (GF_X265EncCtx *)gf_filter_get_udta(filter);
	if (ctx->encoder)
		x265_encoder_close(ctx->encoder);
	if (ctx->param)
		x265_param_free(ctx->param);
}

static const GF_FilterCapability X265EncCaps[] =
	{
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_VISUAL),
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_CODECID, GF_CODECID_RAW),
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_PIXFMT, GF_PIXEL_YUV),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_VISUAL),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_CODECID, GF_CODECID_HEVC),
};

#define OFFS(_n) #_n, offsetof(GF_X265EncCtx, _n)
static GF_FilterArgs X265EncArgs[] =
	{
		{OFFS(bitrate), "target bitrate in kbps", GF_PROP_UINT, "1000", NULL, GF_FS_ARG_HINT_ADVANCED},
		{OFFS(preset), "libx265 speed preset", GF_PROP_STRING, "veryfast",
		 "ultrafast|superfast|veryfast|faster|fast|medium|slow|slower|veryslow", GF_FS_ARG_HINT_ADVANCED},
		{0}};

GF_FilterRegister X265EncRegister = {
	.name = "encx265",
	GF_FS_SET_DESCRIPTION("HEVC encoder (native libx265)")
		GF_FS_SET_HELP("This filter encodes a raw YUV420 video PID to HEVC by calling libx265's "
					   "native API directly (no FFmpeg dependency, unlike ffmpeg-hevc which is "
					   "decode-only in this repo anyway).")
			.private_size = sizeof(GF_X265EncCtx),
	.args = X265EncArgs,
	SETCAPS(X265EncCaps),
	.configure_pid = x265enc_configure_pid,
	.process = x265enc_process,
	.finalize = x265enc_finalize,
};

const GF_FilterRegister * EMSCRIPTEN_KEEPALIVE encx265_register(GF_FilterSession *session)
{
	return &X265EncRegister;
}

#include "filter_register.h"
__attribute__((constructor))
void register_encx265(void) {
    gf_filter_auto_register("encx265", encx265_register);
}
