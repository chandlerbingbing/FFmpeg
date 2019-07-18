/*
 * Copyright (c) 2010 Nicolas George
 * Copyright (c) 2011 Stefano Sabatini
 * Copyright (c) 2014 Andrey Utkin
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/**
 * @file
 * API example for demuxing, decoding, filtering, encoding and muxing
 * @example transcoding.c
 */

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <pthread.h>
#include <string.h>
#include <stdio.h>

static AVFormatContext *ifmt_ctx;
static AVFormatContext **ofmt_ctxs;

typedef struct OptionParserContext {
    char *enc_lib;
    char *filter_desc;
    AVDictionary *enc_opts;
    char *output_file;
    int nb_filters;
    int width;
    int height;
} OptionParserContext;

typedef struct FilteringContext {
    AVFilterContext *buffersink_ctx;
    AVFilterContext *buffersrc_ctx;
    AVFilterGraph *filter_graph;
    unsigned int i;
    unsigned int j;
    // for abr pipeline
    AVFrame *waited_frm;
    AVFrame input_frm;
    pthread_t f_thread;
    pthread_cond_t process_cond;
    pthread_cond_t finish_cond;
    pthread_mutex_t process_mutex;
    pthread_mutex_t finish_mutex;
    int t_end;
    int t_error;
} FilteringContext;
static FilteringContext *filter_ctx;

typedef struct FrameContext{
    AVFrame *frame;
    unsigned int input_stream_index;
    unsigned int count_frame;
} FrameContext;

static AVCodecContext **dec_ctxs;
static AVCodecContext **enc_ctxs;

//config options
static int nb_multi_output = 0;
const int abr_pipeline = 0;

static int open_input_file(const char *filename)
{
    int ret;
    unsigned int i;

    ifmt_ctx = NULL;
    if ((ret = avformat_open_input(&ifmt_ctx, filename, NULL, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot open input file\n");
        return ret;
    }

    if ((ret = avformat_find_stream_info(ifmt_ctx, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot find stream information\n");
        return ret;
    }
    dec_ctxs = av_mallocz_array(ifmt_ctx->nb_streams, sizeof(**dec_ctxs));
    if (!dec_ctxs)
        return AVERROR(ENOMEM);

    for (i = 0; i < ifmt_ctx->nb_streams; i++) {
        AVStream *stream = ifmt_ctx->streams[i];
        AVCodec *dec = avcodec_find_decoder(stream->codecpar->codec_id);
        AVCodecContext *codec_ctx;
        if (!dec) {
            av_log(NULL, AV_LOG_ERROR, "Failed to find decoder for stream #%u\n", i);
            return AVERROR_DECODER_NOT_FOUND;
        }
        codec_ctx = avcodec_alloc_context3(dec);
        if (!codec_ctx) {
            av_log(NULL, AV_LOG_ERROR, "Failed to allocate the decoder context for stream #%u\n", i);
            return AVERROR(ENOMEM);
        }
        ret = avcodec_parameters_to_context(codec_ctx, stream->codecpar);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Failed to copy decoder parameters to input decoder context "
                   "for stream #%u\n", i);
            return ret;
        }
        /* Reencode video & audio and remux subtitles etc. */
        if (codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO
                || codec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO)
                codec_ctx->framerate = av_guess_frame_rate(ifmt_ctx, stream, NULL);
            /* Open decoder */
            ret = avcodec_open2(codec_ctx, dec, NULL);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "Failed to open decoder for stream #%u\n", i);
                return ret;
            }
        }
        dec_ctxs[i] = codec_ctx;
    }

    av_dump_format(ifmt_ctx, 0, filename, 0);
    return 0;
}

static int init_output_file(OptionParserContext *opts_ctxs)
{
    AVStream *out_stream;
    AVStream *in_stream;
    AVCodecContext *dec_ctx, *enc_ctx;
    AVCodec *encoder;
    int ret;
    unsigned int i,j;
    const char *filename;
    enc_ctxs = av_mallocz_array(ifmt_ctx->nb_streams * nb_multi_output, sizeof(**enc_ctxs));

    for(j = 0; j < nb_multi_output; j++) {
        ofmt_ctxs[j] = NULL;
        filename = opts_ctxs[j].output_file;
        avformat_alloc_output_context2(&ofmt_ctxs[j], NULL, NULL, filename);
        if (!ofmt_ctxs[j]) {
            av_log(NULL, AV_LOG_ERROR, "Could not create output context\n");
            return AVERROR_UNKNOWN;
        }

        for (i = 0; i < ifmt_ctx->nb_streams; i++) {
            out_stream = avformat_new_stream(ofmt_ctxs[j], NULL);
            if (!out_stream) {
                av_log(NULL, AV_LOG_ERROR, "Failed allocating output stream\n");
                return AVERROR_UNKNOWN;
            }

            in_stream = ifmt_ctx->streams[i];
            dec_ctx = dec_ctxs[i];

            if (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO
                || dec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
                /* in this example, we choose transcoding to same codec */
                //encoder = avcodec_find_encoder(dec_ctx->codec_id);
                enum AVCodecID enc_codec_id = AV_CODEC_ID_HEVC;
                encoder = avcodec_find_encoder(enc_codec_id);
                if (!encoder) {
                    av_log(NULL, AV_LOG_FATAL, "Necessary encoder not found\n");
                    return AVERROR_INVALIDDATA;
                }
                enc_ctx = avcodec_alloc_context3(encoder);
                if (!enc_ctx) {
                    av_log(NULL, AV_LOG_FATAL, "Failed to allocate the encoder context\n");
                    return AVERROR(ENOMEM);
                }

                /* In this example, we transcode to same properties (picture size,
                 * sample rate etc.). These properties can be changed for output
                 * streams easily using filters */
                if (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO) {
                    enc_ctx->height = opts_ctxs[j].height;
                    enc_ctx->width = opts_ctxs[j].width;
                    enc_ctx->sample_aspect_ratio = dec_ctx->sample_aspect_ratio;
                    enc_ctx->pix_fmt = dec_ctx->pix_fmt;
                    enc_ctx->framerate = dec_ctx->framerate;
                    /* video time_base can be set to whatever is handy and supported by encoder */
                    enc_ctx->time_base = av_inv_q(dec_ctx->framerate);
                } else {
                    enc_ctx->sample_rate = dec_ctx->sample_rate;
                    enc_ctx->channel_layout = dec_ctx->channel_layout;
                    enc_ctx->channels = av_get_channel_layout_nb_channels(enc_ctx->channel_layout);
                    /* take first format from list of supported formats */
                    enc_ctx->sample_fmt = encoder->sample_fmts[0];
                    enc_ctx->time_base = (AVRational) {1, enc_ctx->sample_rate};
                }

                if (ofmt_ctxs[j]->oformat->flags & AVFMT_GLOBALHEADER)
                    enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

                /* Third parameter can be used to pass settings to encoder */
                ret = avcodec_open2(enc_ctx, encoder, &opts_ctxs[j].enc_opts);
                if (ret < 0) {
                    av_log(NULL, AV_LOG_ERROR, "Cannot open video encoder for stream #%u\n", i);
                    return ret;
                }
                ret = avcodec_parameters_from_context(out_stream->codecpar, enc_ctx);
                if (ret < 0) {
                    av_log(NULL, AV_LOG_ERROR, "Failed to copy encoder parameters to output stream #%u\n", i);
                    return ret;
                }

                out_stream->time_base = enc_ctx->time_base;
                out_stream->r_frame_rate = enc_ctx->framerate;
                out_stream->codec->time_base = enc_ctx->time_base;
                enc_ctxs[i * nb_multi_output + j] = enc_ctx;
            } else if (dec_ctx->codec_type == AVMEDIA_TYPE_UNKNOWN) {
                av_log(NULL, AV_LOG_FATAL, "Elementary stream #%d is of unknown type, cannot proceed\n", i);
                return AVERROR_INVALIDDATA;
            } else {
                /* if this stream must be remuxed */
                ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
                if (ret < 0) {
                    av_log(NULL, AV_LOG_ERROR, "Copying parameters for stream #%u failed\n", i);
                    return ret;
                }
                out_stream->time_base = in_stream->time_base;
            }
        }

        av_dump_format(ofmt_ctxs[j], 0, filename, 1);

        if (!(ofmt_ctxs[j]->oformat->flags & AVFMT_NOFILE)) {
            ret = avio_open(&ofmt_ctxs[j]->pb, filename, AVIO_FLAG_WRITE);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "Could not open output file '%s'", filename);
                return ret;
            }
        }

        /* init muxer, write output file header */
        //AVDictionary *opt = NULL;
        //av_dict_set_int(&opt, "video_track_timescale", 60, 0);
        //ret = avformat_write_header(ofmt_ctxs[j], &opt);
        ret = avformat_write_header(ofmt_ctxs[j], NULL);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Error occurred when opening output file\n");
            return ret;
        }
    }
    return 0;
}

static int init_filter(FilteringContext* fctx, AVCodecContext *dec_ctx,
        AVCodecContext *enc_ctx, const char *filter_spec)
{
    char args[512];
    int ret = 0;
    const AVFilter *buffersrc = NULL;
    const AVFilter *buffersink = NULL;
    AVFilterContext *buffersrc_ctx = NULL;
    AVFilterContext *buffersink_ctx = NULL;
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs  = avfilter_inout_alloc();
    AVFilterGraph *filter_graph = avfilter_graph_alloc();

    if (!outputs || !inputs || !filter_graph) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    if(abr_pipeline){
        fctx->f_thread = 0;
    }

    if (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO) {
        buffersrc = avfilter_get_by_name("buffer");
        buffersink = avfilter_get_by_name("buffersink");
        if (!buffersrc || !buffersink) {
            av_log(NULL, AV_LOG_ERROR, "filtering source or sink element not found\n");
            ret = AVERROR_UNKNOWN;
            goto end;
        }

        snprintf(args, sizeof(args),
                "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
                dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt,
                dec_ctx->time_base.num, dec_ctx->time_base.den,
                dec_ctx->sample_aspect_ratio.num,
                dec_ctx->sample_aspect_ratio.den);

        ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in",
                args, NULL, filter_graph);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot create buffer source\n");
            goto end;
        }

        ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out",
                NULL, NULL, filter_graph);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot create buffer sink\n");
            goto end;
        }

        ret = av_opt_set_bin(buffersink_ctx, "pix_fmts",
                (uint8_t*)&enc_ctx->pix_fmt, sizeof(enc_ctx->pix_fmt),
                AV_OPT_SEARCH_CHILDREN);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot set output pixel format\n");
            goto end;
        }
    } else if (dec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
        buffersrc = avfilter_get_by_name("abuffer");
        buffersink = avfilter_get_by_name("abuffersink");
        if (!buffersrc || !buffersink) {
            av_log(NULL, AV_LOG_ERROR, "filtering source or sink element not found\n");
            ret = AVERROR_UNKNOWN;
            goto end;
        }

        if (!dec_ctx->channel_layout)
            dec_ctx->channel_layout =
                av_get_default_channel_layout(dec_ctx->channels);
        snprintf(args, sizeof(args),
                "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=0x%"PRIx64,
                dec_ctx->time_base.num, dec_ctx->time_base.den, dec_ctx->sample_rate,
                av_get_sample_fmt_name(dec_ctx->sample_fmt),
                dec_ctx->channel_layout);
        ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in",
                args, NULL, filter_graph);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot create audio buffer source\n");
            goto end;
        }

        ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out",
                NULL, NULL, filter_graph);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot create audio buffer sink\n");
            goto end;
        }

        ret = av_opt_set_bin(buffersink_ctx, "sample_fmts",
                (uint8_t*)&enc_ctx->sample_fmt, sizeof(enc_ctx->sample_fmt),
                AV_OPT_SEARCH_CHILDREN);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot set output sample format\n");
            goto end;
        }

        ret = av_opt_set_bin(buffersink_ctx, "channel_layouts",
                (uint8_t*)&enc_ctx->channel_layout,
                sizeof(enc_ctx->channel_layout), AV_OPT_SEARCH_CHILDREN);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot set output channel layout\n");
            goto end;
        }

        ret = av_opt_set_bin(buffersink_ctx, "sample_rates",
                (uint8_t*)&enc_ctx->sample_rate, sizeof(enc_ctx->sample_rate),
                AV_OPT_SEARCH_CHILDREN);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot set output sample rate\n");
            goto end;
        }
    } else {
        ret = AVERROR_UNKNOWN;
        goto end;
    }

    /* Endpoints for the filter graph. */
    outputs->name       = av_strdup("in");
    outputs->filter_ctx = buffersrc_ctx;
    outputs->pad_idx    = 0;
    outputs->next       = NULL;

    inputs->name       = av_strdup("out");
    inputs->filter_ctx = buffersink_ctx;
    inputs->pad_idx    = 0;
    inputs->next       = NULL;

    if (!outputs->name || !inputs->name) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    if ((ret = avfilter_graph_parse_ptr(filter_graph, filter_spec,
                    &inputs, &outputs, NULL)) < 0)
        goto end;

    if ((ret = avfilter_graph_config(filter_graph, NULL)) < 0)
        goto end;

    filter_graph->scale_sws_opts = av_strdup("flags=bicubic");
    /* Fill FilteringContext */
    fctx->buffersrc_ctx = buffersrc_ctx;
    fctx->buffersink_ctx = buffersink_ctx;
    fctx->filter_graph = filter_graph;

end:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);

    return ret;
}

static int init_filters(int nb_multi_output, OptionParserContext *opts_ctxs)
{
    const char *filter_spec;
    unsigned int i,j;
    int ret;
    int id;

    filter_ctx = av_malloc_array(ifmt_ctx->nb_streams * nb_multi_output, sizeof(*filter_ctx));
    if (!filter_ctx)
        return AVERROR(ENOMEM);

    for (i = 0; i < ifmt_ctx->nb_streams; i++) {
        for (j = 0; j < nb_multi_output; j++) {
            id = i * nb_multi_output + j;
            filter_ctx[id].buffersrc_ctx = NULL;
            filter_ctx[id].buffersink_ctx = NULL;
            filter_ctx[id].filter_graph = NULL;

            if (!(ifmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO
                  || ifmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO))
                continue;


            if (ifmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
                //filter_spec = "null"; /* passthrough (dummy) filter for video */ //"scale=1920:1080"
                filter_spec = opts_ctxs[j].filter_desc;
            else
                filter_spec = "anull"; /* passthrough (dummy) filter for audio */

            ret = init_filter(&filter_ctx[id], dec_ctxs[i], enc_ctxs[id], filter_spec);
            if (ret)
                return ret;
        }
    }
    return 0;
}

static int encode_write_frame(AVFrame *filt_frame, unsigned int i, int *got_frame, unsigned int j) {
    int ret;
    int got_frame_local;
    AVPacket enc_pkt;
    int (*enc_func)(AVCodecContext *, AVPacket *, const AVFrame *, int *) =
        (ifmt_ctx->streams[i]->codecpar->codec_type ==
         AVMEDIA_TYPE_VIDEO) ? avcodec_encode_video2 : avcodec_encode_audio2;

    if (!got_frame)
        got_frame = &got_frame_local;

    av_log(NULL, AV_LOG_INFO, "[%d]Encoding frame\n",i * nb_multi_output + j);
    /* encode filtered frame */
    enc_pkt.data = NULL;
    enc_pkt.size = 0;
    av_init_packet(&enc_pkt);
    ret = enc_func(enc_ctxs[i * nb_multi_output + j], &enc_pkt,
            filt_frame, got_frame);
    av_frame_free(&filt_frame);
    if (ret < 0)
        return ret;
    if (!(*got_frame))
        return 0;

    /* prepare packet for muxing */
    enc_pkt.stream_index = i;
    av_packet_rescale_ts(&enc_pkt,
                         enc_ctxs[i * nb_multi_output + j]->time_base,
                         ofmt_ctxs[j]->streams[i]->time_base);

    av_log(NULL, AV_LOG_DEBUG, "Muxing frame\n");
    /* mux encoded frame */
    ret = av_interleaved_write_frame(ofmt_ctxs[j], &enc_pkt);
    return ret;
}

static void *filter_pipeline(void *arg) {
    FilteringContext *fl = arg;
    AVFrame *frm;
    int ret;
    unsigned int i = fl->i;
    unsigned int j = fl->j;
    while (1) {
        pthread_mutex_lock(&fl->process_mutex);
        while (fl->waited_frm == NULL && !fl->t_end)
            pthread_cond_wait(&fl->process_cond, &fl->process_mutex);
        pthread_mutex_unlock(&fl->process_mutex);
        if (fl->t_end) break;
        frm = fl->waited_frm;

        ret = av_buffersrc_add_frame_flags(fl->buffersrc_ctx, frm, 0);
        av_log(NULL, AV_LOG_INFO, "\n\n the thread is %d, you can put frame is %ld \n", fl->f_thread,frm->pts);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Error while feeding the filtergraph\n");
        } else {
            AVFrame *filt_frame;
            while (1) {
                filt_frame = av_frame_alloc();
                if (!filt_frame) {
                    ret = AVERROR(ENOMEM);
                    break;
                }
                ret = av_buffersink_get_frame(fl->buffersink_ctx, filt_frame);
                av_log(NULL, AV_LOG_INFO, "\n\n the thread is %d, you can put frame is %ld \n", fl->f_thread,filt_frame->pts);
                if (ret < 0) {
                    /* if no more frames for output - returns AVERROR(EAGAIN)
                     * if flushed and no more frames for output - returns AVERROR_EOF
                     * rewrite retcode to 0 to show it as normal procedure completion
                     */
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                        ret = 0;
                    av_frame_free(&filt_frame);
                    break;
                }

                filt_frame->pict_type = AV_PICTURE_TYPE_NONE;
                ret = encode_write_frame(filt_frame, i, NULL, j);
                if (ret < 0)
                    break;
            }
        }
        fl->t_error = ret;

        pthread_mutex_lock(&fl->finish_mutex);
        fl->waited_frm = NULL;
        pthread_cond_signal(&fl->finish_cond);
        pthread_mutex_unlock(&fl->finish_mutex);

        if (ret < 0)
            break;
    }
    return;
}

static int filter_encode_write_frame(FrameContext *frame_ctx) {
    int ret;
    unsigned int j;
    AVFrame *decoded_frame = frame_ctx->frame;
    AVFrame *frame;
    frame = av_frame_alloc();
    unsigned int i = frame_ctx->input_stream_index;
    int count_frame = frame_ctx->count_frame;

    av_log(NULL, AV_LOG_INFO, "\n\n %d:Pushing decoded frame to filters \n", count_frame);
    /* push the decoded frame into the filtergraph */
    for (j = 0; j < nb_multi_output; j++) {
        int filter_id = i * nb_multi_output + j;

        if (!abr_pipeline) {

            if ((j < nb_multi_output - 1) && decoded_frame) {
                ret = av_frame_ref(frame, decoded_frame);
                if(ret < 0){
                    break;
                }
            } else {
                frame = decoded_frame;
            }

            ret = av_buffersrc_add_frame_flags(filter_ctx[filter_id].buffersrc_ctx, frame, 0);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "Error while feeding the filtergraph\n");
                return ret;
            }

            /* pull filtered frames from the filtergraph */
            AVFrame *filt_frame;
            while (1) {
                filt_frame = av_frame_alloc();
                if (!filt_frame) {
                    ret = AVERROR(ENOMEM);
                    break;
                }
                av_log(NULL, AV_LOG_INFO, "[%d]Pulling filtered frame from filters\n", filter_id);
                ret = av_buffersink_get_frame(filter_ctx[filter_id].buffersink_ctx, filt_frame);
                if (ret < 0) {
                    /* if no more frames for output - returns AVERROR(EAGAIN)
                     * if flushed and no more frames for output - returns AVERROR_EOF
                     * rewrite retcode to 0 to show it as normal procedure completion
                     */
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                        ret = 0;
                    av_frame_free(&filt_frame);
                    break;
                }

                filt_frame->pict_type = AV_PICTURE_TYPE_NONE;
                ret = encode_write_frame(filt_frame, i, NULL, j);
                if (ret < 0)
                    break;
            }
        } else {
            frame = &filter_ctx[filter_id].input_frm;
            if ((j < nb_multi_output - 1) && decoded_frame) {
                ret = av_frame_ref(frame, decoded_frame);
                if(ret < 0){
                    break;
                }
            } else {
                frame = decoded_frame;
            }
            filter_ctx[filter_id].i = i;
            filter_ctx[filter_id].j = j;

            if(filter_ctx[filter_id].f_thread == 0){
                if ((ret = pthread_create(&filter_ctx[filter_id].f_thread, NULL, filter_pipeline, &filter_ctx[filter_id]))) {
                    av_log(NULL, AV_LOG_ERROR, "pthread_create failed: %s. Try to increase `ulimit -v` or decrease `ulimit -s`.\n", strerror(ret));
                    return AVERROR(ret);
                }
                pthread_mutex_init(&filter_ctx[filter_id].process_mutex, NULL);
                pthread_mutex_init(&filter_ctx[filter_id].finish_mutex, NULL);
                pthread_cond_init(&filter_ctx[filter_id].process_cond, NULL);
                pthread_cond_init(&filter_ctx[filter_id].finish_cond, NULL);
                filter_ctx[filter_id].t_end = 0;
                filter_ctx[filter_id].t_error = 0;
            }
            pthread_mutex_lock(&filter_ctx[filter_id].process_mutex);
            filter_ctx[filter_id].waited_frm = frame;
            pthread_cond_signal(&filter_ctx[filter_id].process_cond);
            pthread_mutex_unlock(&filter_ctx[filter_id].process_mutex);
        }
    }

    if (abr_pipeline) {
        for (j = 0; j < nb_multi_output; j++) {
            int filter_id = i * nb_multi_output + j;
            pthread_mutex_lock(&filter_ctx[filter_id].finish_mutex);
            while (filter_ctx[filter_id].waited_frm != NULL)
                pthread_cond_wait(&filter_ctx[filter_id].finish_cond, &filter_ctx[filter_id].finish_mutex);
            pthread_mutex_unlock(&filter_ctx[filter_id].finish_mutex);
        }
        for (j = 0; j < nb_multi_output; j++) {
            int filter_id = i * nb_multi_output + j;
            if (filter_ctx[filter_id].t_error < 0) {
                ret = filter_ctx[filter_id].t_error;
                break;
            }
        }
    }

    return ret;
}

static int flush_encoder(unsigned int i, unsigned int j)
{
    int ret;
    int got_frame;

    if (!(enc_ctxs[i * nb_multi_output + j]->codec->capabilities &
                AV_CODEC_CAP_DELAY))
        return 0;

    while (1) {
        av_log(NULL, AV_LOG_INFO, "Flushing stream #%u encoder\n", i);
        ret = encode_write_frame(NULL, i, &got_frame, j);
        if (ret < 0)
            break;
        if (!got_frame)
            return 0;
    }
    return ret;
}


int main(int argc, char **argv) {
    int ret;
    AVPacket packet = {.data = NULL, .size = 0};
    enum AVMediaType type;
    int skipped_frame = 0;
    int got_frame;
    int (*dec_func)(AVCodecContext *, AVFrame *, int *, const AVPacket *);
    FrameContext frame_ctx;
    frame_ctx.count_frame = 0;
    const char *input_file;
    OptionParserContext *opts_ctxs;

    if (argc < 3) {
        av_log(NULL, AV_LOG_ERROR, "Usage: %s -i <input file> \ \n "
                                   "-nb_outputs 4 \ \n"
                                   "-c:v libsvt_hevc -vf scale=7680:3840 -tune 1 -preset 9 -o o1.mp4 \ \n"
                                   "-c:v libsvt_hevc -vf scale=1920:1080 -tune 1 -preset 9 -o o2.mp4 \ \n"
                                   "-c:v libsvt_hevc -vf scale=1280:720 -tune 1 -preset 9 -o o3.mp4 \ \n"
                                   "-c:v libsvt_hevc -vf scale=480:360 -tune 1 -preset 9 -o o4.mp4 \ \n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "-i") == 0) {
        input_file = argv[2];
    } else{
        av_log(NULL, AV_LOG_ERROR, "please provide input file. Usage: -i <input file>\n");
        return 1;
    }

    if (strcmp(argv[3], "-nb_outputs") == 0) {
        nb_multi_output = atoi(argv[4]);
    } else {
        av_log(NULL, AV_LOG_ERROR, "please provide the number of outputs. Usage: -nb_outputs 4\n");
        return 1;
    }

    opts_ctxs = (OptionParserContext *)malloc(nb_multi_output * sizeof(OptionParserContext));
    if (opts_ctxs == NULL) {
        av_log(NULL, AV_LOG_ERROR, "optx_ctxs malloc failed\n");
        return 1;
    }

    int j = 0;
    for(int i = 1;i < argc;i=i+2) {
        if (strcmp(argv[i], "-c:v") == 0) {
            opts_ctxs[j].enc_lib = argv[i + 1];
        } else if (strcmp(argv[i], "-vf") == 0) {
            opts_ctxs[j].filter_desc = argv[i + 1];
            char vf_arg[20];
            strcpy(vf_arg, argv[i+1]);
            strtok(vf_arg,"=");
            opts_ctxs[j].width = atoi(strtok(NULL, ":"));
            opts_ctxs[j].height = atoi(strtok(NULL, ":"));
        } else if (strcmp(argv[i], "-rc") == 0) {
            av_dict_set(&opts_ctxs[j].enc_opts,"rc","1", 0);
        } else if (strcmp(argv[i], "-g") == 0) {
            av_dict_set(&opts_ctxs[j].enc_opts,"g",argv[i+1], 0);
        } else if (strcmp(argv[i], "-b:v") == 0) {
            av_dict_set(&opts_ctxs[j].enc_opts,"b",argv[i+1], 0);
        } else if (strcmp(argv[i], "-tune") == 0) {
            av_dict_set(&opts_ctxs[j].enc_opts,"tune",argv[i+1], 0);
        } else if (strcmp(argv[i], "-preset") == 0) {
            av_dict_set(&opts_ctxs[j].enc_opts,"preset",argv[i+1], 0);
        } else if (strcmp(argv[i], "-o") == 0) {
            opts_ctxs[j].output_file = argv[i + 1];
            j++;
        }
    }

    if ((ret = open_input_file(input_file)) < 0)
        goto end;

    ofmt_ctxs = av_mallocz_array(nb_multi_output, sizeof(**ofmt_ctxs));

    if ((ret = init_output_file(opts_ctxs)) < 0)
        goto end;

    if ((ret = init_filters(nb_multi_output, opts_ctxs)) < 0)
        goto end;

    // read all packets
    while (1) {
        unsigned int i;

        if ((ret = av_read_frame(ifmt_ctx, &packet)) < 0)
            break;

        frame_ctx.input_stream_index = packet.stream_index;
        i = packet.stream_index;

        type = ifmt_ctx->streams[i]->codecpar->codec_type;
        av_log(NULL, AV_LOG_DEBUG, "Demuxer gave frame of stream_index %u\n", i);

        if (filter_ctx[i * nb_multi_output].filter_graph) {

            av_log(NULL, AV_LOG_DEBUG, "Going to reencode&filter the frame\n");
            frame_ctx.frame = av_frame_alloc();
            if (!frame_ctx.frame) {
                ret = AVERROR(ENOMEM);
                break;
            }
            av_packet_rescale_ts(&packet,
                                 ifmt_ctx->streams[i]->time_base,
                                 dec_ctxs[i]->time_base);
            dec_func = (type == AVMEDIA_TYPE_VIDEO) ? avcodec_decode_video2 :
                       avcodec_decode_audio4;
            ret = dec_func(dec_ctxs[i], frame_ctx.frame,
                           &got_frame, &packet);
            if (ret < 0) {
                av_frame_free(&frame_ctx.frame);
                av_log(NULL, AV_LOG_ERROR, "Decoding failed\n");
                break;
            }

            if (got_frame) {
                frame_ctx.count_frame++;
                frame_ctx.frame->pts = frame_ctx.frame->best_effort_timestamp;
                ret = filter_encode_write_frame(&frame_ctx);
                av_frame_free(&frame_ctx.frame);
                if (ret < 0)
                    goto end;
            } else {
                skipped_frame++;
                av_frame_free(&frame_ctx.frame);
            }

        } else {
            // remux this frame without reencoding
            for (int j = 0; j < nb_multi_output; j++) {
                av_packet_rescale_ts(&packet,
                                     ifmt_ctx->streams[i]->time_base,
                                     ofmt_ctxs[j]->streams[i]->time_base);

                ret = av_interleaved_write_frame(ofmt_ctxs[j], &packet);
            }
            if (ret < 0)
                goto end;
        }
        av_packet_unref(&packet);
    }

    // flush decoders
    for (int stream_id = 0; stream_id < ifmt_ctx->nb_streams; stream_id++) {
        for (int i = skipped_frame; i > 0; i--) {
            frame_ctx.input_stream_index = stream_id;
            frame_ctx.frame = av_frame_alloc();
            if (!frame_ctx.frame) {
                ret = AVERROR(ENOMEM);
                break;
            }
            type = ifmt_ctx->streams[stream_id]->codecpar->codec_type;
            dec_func = (type == AVMEDIA_TYPE_VIDEO) ? avcodec_decode_video2 :
                       avcodec_decode_audio4;
            ret = dec_func(dec_ctxs[stream_id], frame_ctx.frame,
                           &got_frame, &packet);
            if (ret < 0) {
                av_frame_free(&frame_ctx.frame);
                av_log(NULL, AV_LOG_ERROR, "Decoding failed\n");
                break;
            }

            if (got_frame) {
                frame_ctx.count_frame++;
                frame_ctx.frame->pts = frame_ctx.frame->best_effort_timestamp;
                ret = filter_encode_write_frame(&frame_ctx);
                av_frame_free(&frame_ctx.frame);
                if (ret < 0)
                    goto end;
            } else {
                av_frame_free(&frame_ctx.frame);
            }
        }
    }
    // flush filters and encoders
    for (int i = 0; i < ifmt_ctx->nb_streams; i++) {
        // flush filter
        if (!filter_ctx[i * nb_multi_output].filter_graph)
            continue;

        frame_ctx.frame = NULL;
        ret = filter_encode_write_frame(&frame_ctx);

        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Flushing filter failed\n");
            goto end;
        }

        // flush encoder
        for (int j = 0; j < nb_multi_output; j++) {
            ret = flush_encoder(i, j);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "Flushing encoder failed\n");
                goto end;
            }
        }
    }

    for (int j = 0; j < nb_multi_output; j++) {
        av_write_trailer(ofmt_ctxs[j]);
    }

    end:
    free(opts_ctxs);
    av_packet_unref(&packet);
    av_frame_free(&frame_ctx.frame);
    for (int i = 0; i < ifmt_ctx->nb_streams; i++) {
        avcodec_free_context(&dec_ctxs[i]);
        for (int j = 0; j < nb_multi_output; j++) {
            int id = i * nb_multi_output + j;
            if (ofmt_ctxs[j] && ofmt_ctxs[j]->nb_streams > i && ofmt_ctxs[j]->streams[i] && enc_ctxs[id])
                avcodec_free_context(&enc_ctxs[id]);
            if (filter_ctx && filter_ctx[id].filter_graph)
                avfilter_graph_free(&filter_ctx[id].filter_graph);
        }
    }
    av_free(filter_ctx);
    av_free(dec_ctxs);
    av_free(enc_ctxs);
    avformat_close_input(&ifmt_ctx);
    for (int j = 0; j < nb_multi_output; j++) {
        if (ofmt_ctxs[j] && !(ofmt_ctxs[j]->oformat->flags & AVFMT_NOFILE))
            avio_closep(&ofmt_ctxs[j]->pb);
        avformat_free_context(ofmt_ctxs[j]);
    }
    if (ret < 0)
        av_log(NULL, AV_LOG_ERROR, "Error occurred: %s\n", av_err2str(ret));

    return ret ? 1 : 0;
}