/*
 * Butchies Web-Player – Decoder-Kern (FFmpeg als WebAssembly).
 * Läuft in einem Web Worker im Browser: liest die Originaldatei per HTTP-Range direkt vom NAS
 * (Direct Stream), entschlüsselt Bild (x265/HEVC, H.264 …) und Ton (AC3, E-AC3, DTS, TrueHD …)
 * auf dem PC des Zuschauers. Das NAS rechnet nichts um.
 * Lizenz: LGPL-2.1 (wie die verwendeten FFmpeg-Teile).
 */
#include <emscripten.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <stdio.h>
#include <string.h>

/* JS liefert die Bytes der Datei (synchrone Range-Abfrage im Worker). */
EM_JS(int, js_read, (double pos, uint8_t *buf, int size), {
  return Module.bmcRead(pos, buf, size);
});
EM_JS(double, js_size, (void), {
  return Module.bmcSize();
});

#define OUT_RATE 48000

typedef struct {
    AVFormatContext *fmt;
    AVIOContext *avio;
    int64_t pos, size;
    AVCodecContext *vdec, *adec;
    int vidx, aidx, threads;
    AVPacket *pkt;
    AVFrame *vfrm, *afrm, *conv;
    struct SwsContext *sws;
    SwrContext *swr;
    float *abuf;
    int abuf_cap, a_samples;
    double a_pts, v_pts;
    int v_pending, a_pending, eof, v_done, a_done;
    int out_bits;            /* 8 oder 10 (16-Bit-Ebenen) */
    char info[32768];
} Player;

static Player P;

static int rd(void *o, uint8_t *buf, int n) {
    (void)o;
    if (P.pos >= P.size) return AVERROR_EOF;
    int r = js_read((double)P.pos, buf, n);
    if (r <= 0) return AVERROR_EOF;
    P.pos += r;
    return r;
}

static int64_t sk(void *o, int64_t off, int wh) {
    (void)o;
    if (wh & AVSEEK_SIZE) return P.size;
    wh &= ~AVSEEK_FORCE;
    if (wh == SEEK_SET) P.pos = off;
    else if (wh == SEEK_CUR) P.pos += off;
    else if (wh == SEEK_END) P.pos = P.size + off;
    else return -1;
    return P.pos;
}

static double ts(AVStream *st, int64_t t) {
    if (t == AV_NOPTS_VALUE) return -1;
    return t * av_q2d(st->time_base);
}

static void jesc(char *dst, size_t cap, const char *s) {
    size_t n = 0;
    for (; s && *s && n + 7 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { dst[n++] = '\\'; dst[n++] = c; }
        else if (c < 0x20) { n += snprintf(dst + n, cap - n, "\\u%04x", c); }
        else dst[n++] = c;
    }
    dst[n] = 0;
}

static int open_decoder(int idx, AVCodecContext **out, int threads) {
    AVStream *st = P.fmt->streams[idx];
    const AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) return -1;
    AVCodecContext *c = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(c, st->codecpar);
    c->pkt_timebase = st->time_base;
    if (threads > 1) {
        c->thread_count = threads;
        c->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    } else {
        c->thread_count = 1;
    }
    if (avcodec_open2(c, dec, NULL) < 0) { avcodec_free_context(&c); return -1; }
    *out = c;
    return 0;
}

static void close_audio(void) {
    if (P.adec) avcodec_free_context(&P.adec);
    if (P.swr) swr_free(&P.swr);
    P.a_pending = 0;
}

EMSCRIPTEN_KEEPALIVE int bmc_set_audio(int idx) {
    close_audio();
    P.aidx = -1;
    if (idx < 0 || idx >= (int)P.fmt->nb_streams) return 0;
    if (P.fmt->streams[idx]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) return -1;
    if (open_decoder(idx, &P.adec, 1) < 0) return -2;
    P.aidx = idx;
    for (unsigned i = 0; i < P.fmt->nb_streams; i++)
        if (P.fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
            P.fmt->streams[i]->discard = ((int)i == idx) ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
    return 0;
}

EMSCRIPTEN_KEEPALIVE const char *bmc_info(void) { return P.info; }

EMSCRIPTEN_KEEPALIVE int bmc_open(int threads) {
    memset(&P, 0, sizeof(P));
    P.vidx = P.aidx = -1;
    P.threads = threads;
    P.size = (int64_t)js_size();
    if (P.size <= 0) return -1;
    uint8_t *b = av_malloc(1 << 17);
    P.avio = avio_alloc_context(b, 1 << 17, 0, NULL, rd, NULL, sk);
    P.fmt = avformat_alloc_context();
    P.fmt->pb = P.avio;
    P.fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
    AVDictionary *o = NULL;
    av_dict_set(&o, "probesize", "4000000", 0);
    av_dict_set(&o, "analyzeduration", "3000000", 0);
    if (avformat_open_input(&P.fmt, NULL, NULL, &o) < 0) { av_dict_free(&o); return -2; }
    av_dict_free(&o);
    if (avformat_find_stream_info(P.fmt, NULL) < 0) return -3;
    P.vidx = av_find_best_stream(P.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    int a = av_find_best_stream(P.fmt, AVMEDIA_TYPE_AUDIO, -1, P.vidx, NULL, 0);
    for (unsigned i = 0; i < P.fmt->nb_streams; i++) {
        int t = P.fmt->streams[i]->codecpar->codec_type;
        if ((int)i != P.vidx && t != AVMEDIA_TYPE_AUDIO) P.fmt->streams[i]->discard = AVDISCARD_ALL;
    }
    if (P.vidx >= 0 && open_decoder(P.vidx, &P.vdec, threads) < 0) return -4;
    if (a >= 0) bmc_set_audio(a);
    P.pkt = av_packet_alloc();
    P.vfrm = av_frame_alloc();
    P.afrm = av_frame_alloc();
    P.conv = av_frame_alloc();

    /* Infos als JSON */
    size_t n = 0, cap = sizeof(P.info);
    double dur = P.fmt->duration > 0 ? P.fmt->duration / (double)AV_TIME_BASE : 0;
    n += snprintf(P.info + n, cap - n, "{\"duration\":%.3f,\"video\":%d,\"audio\":%d,\"streams\":[", dur, P.vidx, P.aidx);
    for (unsigned i = 0; i < P.fmt->nb_streams && n < cap - 600; i++) {
        AVStream *st = P.fmt->streams[i];
        AVCodecParameters *cp = st->codecpar;
        const char *type = cp->codec_type == AVMEDIA_TYPE_VIDEO ? "video" : cp->codec_type == AVMEDIA_TYPE_AUDIO ? "audio"
                         : cp->codec_type == AVMEDIA_TYPE_SUBTITLE ? "subtitle" : "other";
        AVDictionaryEntry *lang = av_dict_get(st->metadata, "language", NULL, 0);
        AVDictionaryEntry *title = av_dict_get(st->metadata, "title", NULL, 0);
        char l[64] = "", ti[300] = "";
        jesc(l, sizeof l, lang ? lang->value : "");
        jesc(ti, sizeof ti, title ? title->value : "");
        n += snprintf(P.info + n, cap - n, "%s{\"index\":%u,\"type\":\"%s\",\"codec\":\"%s\",\"lang\":\"%s\",\"title\":\"%s\",\"channels\":%d,\"width\":%d,\"height\":%d,\"default\":%d,\"decodable\":%d}",
                      i ? "," : "", i, type, avcodec_get_name(cp->codec_id), l, ti, cp->ch_layout.nb_channels, cp->width, cp->height,
                      (st->disposition & AV_DISPOSITION_DEFAULT) ? 1 : 0, avcodec_find_decoder(cp->codec_id) ? 1 : 0);
    }
    snprintf(P.info + n, cap - n, "]}");
    return 0;
}

/* --- Bild ------------------------------------------------------------------------------------ */
static AVFrame *outv;
EMSCRIPTEN_KEEPALIVE int bmc_v_width(void) { return outv ? outv->width : 0; }
EMSCRIPTEN_KEEPALIVE int bmc_v_height(void) { return outv ? outv->height : 0; }
EMSCRIPTEN_KEEPALIVE int bmc_v_bits(void) { return P.out_bits; }
EMSCRIPTEN_KEEPALIVE uint8_t *bmc_v_data(int pl) { return outv ? outv->data[pl] : 0; }
EMSCRIPTEN_KEEPALIVE int bmc_v_linesize(int pl) { return outv ? outv->linesize[pl] : 0; }
EMSCRIPTEN_KEEPALIVE double bmc_v_pts(void) { return P.v_pts; }
EMSCRIPTEN_KEEPALIVE int bmc_v_matrix(void) { return P.vdec ? (int)P.vdec->colorspace : 0; }
EMSCRIPTEN_KEEPALIVE int bmc_v_range(void) { return P.vdec ? (int)P.vdec->color_range : 0; }

static int emit_video(void) {
    AVFrame *f = P.vfrm;
    P.v_pts = ts(P.fmt->streams[P.vidx], f->best_effort_timestamp);
    if (f->format == AV_PIX_FMT_YUV420P || f->format == AV_PIX_FMT_YUVJ420P) { outv = f; P.out_bits = 8; return 1; }
    if (f->format == AV_PIX_FMT_YUV420P10LE) { outv = f; P.out_bits = 10; return 1; }
    /* alles andere -> 8-Bit 4:2:0 */
    if (!P.sws || P.conv->width != f->width || P.conv->height != f->height) {
        sws_freeContext(P.sws);
        P.sws = sws_getContext(f->width, f->height, f->format, f->width, f->height, AV_PIX_FMT_YUV420P, SWS_BILINEAR, NULL, NULL, NULL);
        av_frame_unref(P.conv);
        P.conv->format = AV_PIX_FMT_YUV420P; P.conv->width = f->width; P.conv->height = f->height;
        av_frame_get_buffer(P.conv, 32);
    }
    sws_scale(P.sws, (const uint8_t *const *)f->data, f->linesize, 0, f->height, P.conv->data, P.conv->linesize);
    outv = P.conv; P.out_bits = 8;
    return 1;
}

/* --- Ton ------------------------------------------------------------------------------------- */
EMSCRIPTEN_KEEPALIVE float *bmc_a_data(void) { return P.abuf; }
EMSCRIPTEN_KEEPALIVE int bmc_a_samples(void) { return P.a_samples; }
EMSCRIPTEN_KEEPALIVE double bmc_a_pts(void) { return P.a_pts; }

static int emit_audio(void) {
    AVFrame *f = P.afrm;
    if (!P.swr) {
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        swr_alloc_set_opts2(&P.swr, &stereo, AV_SAMPLE_FMT_FLT, OUT_RATE, &f->ch_layout, f->format, f->sample_rate, 0, NULL);
        if (f->ch_layout.nb_channels > 2) av_opt_set_double(P.swr, "center_mix_level", 0.8, 0); /* Sprache gut hörbar */
        if (swr_init(P.swr) < 0) { swr_free(&P.swr); return 0; }
    }
    int max = (int)av_rescale_rnd(swr_get_delay(P.swr, f->sample_rate) + f->nb_samples, OUT_RATE, f->sample_rate, AV_ROUND_UP);
    if (max * 2 > P.abuf_cap) { av_free(P.abuf); P.abuf_cap = max * 2 + 4096; P.abuf = av_malloc(P.abuf_cap * sizeof(float)); }
    uint8_t *out[1] = { (uint8_t *)P.abuf };
    int got = swr_convert(P.swr, out, max, (const uint8_t **)f->extended_data, f->nb_samples);
    if (got <= 0) return 0;
    P.a_samples = got;
    P.a_pts = ts(P.fmt->streams[P.aidx], f->best_effort_timestamp);
    return 2;
}

/* --- Hauptschritt: 1 = Bild bereit, 2 = Ton bereit, -1 = Ende, <-1 Fehler --------------------- */
EMSCRIPTEN_KEEPALIVE int bmc_step(void) {
    for (int guard = 0; guard < 4000; guard++) {
        if (P.v_pending) {
            int r = avcodec_receive_frame(P.vdec, P.vfrm);
            if (r == 0) return emit_video();
            P.v_pending = 0;
            if (r == AVERROR_EOF) P.v_done = 1;
        }
        if (P.a_pending && P.adec) {
            int r = avcodec_receive_frame(P.adec, P.afrm);
            if (r == 0) { int e = emit_audio(); if (e) return e; continue; }
            P.a_pending = 0;
            if (r == AVERROR_EOF) P.a_done = 1;
        }
        if (P.eof) {
            if ((P.v_done || !P.vdec) && (P.a_done || !P.adec)) return -1;
            continue;
        }
        int r = av_read_frame(P.fmt, P.pkt);
        if (r < 0) {
            P.eof = 1;
            if (P.vdec) { avcodec_send_packet(P.vdec, NULL); P.v_pending = 1; }
            if (P.adec) { avcodec_send_packet(P.adec, NULL); P.a_pending = 1; }
            continue;
        }
        if (P.pkt->stream_index == P.vidx && P.vdec) {
            if (avcodec_send_packet(P.vdec, P.pkt) >= 0) P.v_pending = 1;
        } else if (P.pkt->stream_index == P.aidx && P.adec) {
            if (avcodec_send_packet(P.adec, P.pkt) >= 0) P.a_pending = 1;
        }
        av_packet_unref(P.pkt);
    }
    return 0;
}

EMSCRIPTEN_KEEPALIVE int bmc_seek(double sec) {
    int64_t t = (int64_t)(sec * AV_TIME_BASE);
    int r = avformat_seek_file(P.fmt, -1, INT64_MIN, t, t, 0);
    if (r < 0) r = av_seek_frame(P.fmt, -1, t, AVSEEK_FLAG_BACKWARD);
    if (P.vdec) avcodec_flush_buffers(P.vdec);
    if (P.adec) avcodec_flush_buffers(P.adec);
    if (P.swr) swr_free(&P.swr);
    P.eof = P.v_done = P.a_done = 0;
    P.v_pending = P.a_pending = 0;
    return r;
}
