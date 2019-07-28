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
#include <libavutil/time.h>
#include <unistd.h>

//config parse options
static int nb_multi_output = 1;
static char *input_file;
typedef struct OptionParserContext {
    char *enc_lib;
    char *filter_desc;
    AVDictionary *enc_opts;
    char *output_file;
    int nb_filters;
    int width;
    int height;
} OptionParserContext;
OptionParserContext *opts_ctxs;

//input context
static AVFormatContext *ifmt_ctx;
static AVCodecContext *dec_ctx;
static int video_stream_index = -1;

//output context
static AVFormatContext **ofmt_ctxs;
static AVCodecContext **enc_ctxs;

//filter context
typedef struct FilteringContext {
    AVFilterContext *buffersink_ctx;
    AVFilterContext *buffersrc_ctx;
    AVFilterGraph *filter_graph;
    unsigned int i;
    unsigned int j;
} FilteringContext;
static FilteringContext *filter_ctxs;

//config multi_threads
#define BUF_SIZE 5

static AVFrame *waited_frm[BUF_SIZE];
static int ref_count[BUF_SIZE];
static pthread_cond_t process_cond;
static pthread_mutex_t process_mutex;
static int t_end; //todo
static int t_error; //todo

typedef struct ProducerContext {
    int prod_tail;
    AVFrame *input_frm;
    pthread_t f_thread;
} ProducerContext;
static ProducerContext *prod_ctx;

typedef struct ConsumerContext {
    int cons_head;
    AVFrame *decoded_frame;
    AVFrame *filtered_frame;
    pthread_t f_thread;
} ConsumerContext;
static ConsumerContext *cons_ctxs;


static int parse_options(int argc, char **argv){
    int ret = 0;

    if (argc < 3) {
        av_log(NULL, AV_LOG_ERROR, "Usage: %s -i <input file> -nb_outputs 4 \ \n"
                                   "-c:v libsvt_hevc -vf scale=7680:3840 -rc 1 -b:v 13M -g 60 -tune 1 -preset 6 -o o1.mp4 \ \n"
                                   "-c:v libsvt_hevc -vf scale=1920:1080 -rc 1 -b:v 4.5M -g 60 -tune 1 -preset 6 -o o2.mp4 \ \n"
                                   "-c:v libsvt_hevc -vf scale=1280:720 -rc 1 -b:v 2M   -g 60 -tune 1 -preset 6 -o o3.mp4 \ \n"
                                   "-c:v libsvt_hevc -vf scale=480:360  -rc 1 -b:v 0.5M -g 60 -tune 1 -preset 6 -o o4.mp4 \ \n", argv[0]);
        return -1;
    }

    if (argc < 3) {
        av_log(NULL, AV_LOG_ERROR, "Usage: %s -i <input file> -nb_outputs 4 \ \n"
                                   "-c:v libsvt_hevc -vf scale=7680:3840 -rc 1 -b:v 13M -g 60 -tune 1 -preset 6 -o o1.mp4 \ \n"
                                   "-c:v libsvt_hevc -vf scale=1920:1080 -rc 1 -b:v 4.5M -g 60 -tune 1 -preset 6 -o o2.mp4 \ \n"
                                   "-c:v libsvt_hevc -vf scale=1280:720 -rc 1 -b:v 2M   -g 60 -tune 1 -preset 6 -o o3.mp4 \ \n"
                                   "-c:v libsvt_hevc -vf scale=480:360  -rc 1 -b:v 0.5M -g 60 -tune 1 -preset 6 -o o4.mp4 \ \n", argv[0]);
        return -1;
    }

    if (strcmp(argv[1], "-i") == 0) {
        input_file = argv[2];
    } else{
        av_log(NULL, AV_LOG_ERROR, "please provide input file. Usage: -i <input file>\n");
        return -1;
    }

    if (strcmp(argv[3], "-nb_outputs") == 0) {
        nb_multi_output = atoi(argv[4]);
    } else {
        av_log(NULL, AV_LOG_ERROR, "please provide the number of outputs. Usage: -nb_outputs 4\n");
        return -1;
    }

    opts_ctxs = (OptionParserContext *)malloc(nb_multi_output * sizeof(OptionParserContext));
    if (opts_ctxs == NULL) {
        av_log(NULL, AV_LOG_ERROR, "optx_ctxs malloc failed\n");
        return -1;
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
    return ret;
}

static int open_input_file(void)
{
    int ret;
    const char *filename = input_file;
    AVCodec *dec;

    ifmt_ctx = NULL;
    if ((ret = avformat_open_input(&ifmt_ctx, filename, NULL, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot open input file\n");
        return ret;
    }

    if ((ret = avformat_find_stream_info(ifmt_ctx, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot find stream information\n");
        return ret;
    }

    /* select the video stream */
    ret = av_find_best_stream(ifmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot find a video stream in the input file\n");
        return ret;
    }
    video_stream_index = ret;

    AVStream *stream = ifmt_ctx->streams[video_stream_index];
    dec_ctx = avcodec_alloc_context3(dec);
    if (!dec_ctx) {
        av_log(NULL, AV_LOG_ERROR, "Failed to allocate the decoder context for stream #%u\n", video_stream_index);
        return AVERROR(ENOMEM);
    }
    ret = avcodec_parameters_to_context(dec_ctx, stream->codecpar);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Failed to copy decoder parameters to input decoder context "
                                   "for stream #%u\n", video_stream_index);
        return ret;
    }
    dec_ctx->framerate = av_guess_frame_rate(ifmt_ctx, stream, NULL);
    AVDictionary *dec_opt = NULL;
    //av_dict_set(&dec_opt,"threads","1", 0);
    av_dict_set(&dec_opt, "refcounted_frames", "1", 0);

    ret = avcodec_open2(dec_ctx, dec, &dec_opt);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Failed to open decoder for stream #%u\n", video_stream_index);
        return ret;
    }

    av_dump_format(ifmt_ctx, 0, filename, 0);
    return 0;
}

static int init_output_file(void) {
    AVStream *out_stream;
    AVStream *in_stream;
    AVCodecContext *enc_ctx;
    AVCodec *encoder;
    int ret;
    unsigned int j;
    const char *filename;

    ofmt_ctxs = av_mallocz_array(nb_multi_output, sizeof(**ofmt_ctxs));
    if (!ofmt_ctxs)
        return AVERROR(ENOMEM);

    enc_ctxs = av_mallocz_array(nb_multi_output, sizeof(**enc_ctxs));
    if (!enc_ctxs)
        return AVERROR(ENOMEM);

    for (j = 0; j < nb_multi_output; j++) {
        ofmt_ctxs[j] = NULL;
        filename = opts_ctxs[j].output_file;
        avformat_alloc_output_context2(&ofmt_ctxs[j], NULL, NULL, filename);
        if (!ofmt_ctxs[j]) {
            av_log(NULL, AV_LOG_ERROR, "Could not create output context\n");
            return AVERROR_UNKNOWN;
        }

        out_stream = avformat_new_stream(ofmt_ctxs[j], NULL);
        if (!out_stream) {
            av_log(NULL, AV_LOG_ERROR, "Failed allocating output stream\n");
            return AVERROR_UNKNOWN;
        }

        in_stream = ifmt_ctx->streams[video_stream_index];

        enum AVCodecID enc_codec_id = AV_CODEC_ID_HEVC; //todo: can be modified later
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

        enc_ctx->height = opts_ctxs[j].height;
        enc_ctx->width = opts_ctxs[j].width;
        enc_ctx->sample_aspect_ratio = dec_ctx->sample_aspect_ratio;
        enc_ctx->pix_fmt = dec_ctx->pix_fmt;
        enc_ctx->framerate = dec_ctx->framerate;
        /* video time_base can be set to whatever is handy and supported by encoder */
        enc_ctx->time_base = av_inv_q(dec_ctx->framerate);

        if (ofmt_ctxs[j]->oformat->flags & AVFMT_GLOBALHEADER)
            enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

        /* Third parameter can be used to pass settings to encoder */
        ret = avcodec_open2(enc_ctx, encoder, &opts_ctxs[j].enc_opts);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot open video encoder for stream #%u\n", video_stream_index);
            return ret;
        }
        ret = avcodec_parameters_from_context(out_stream->codecpar, enc_ctx);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Failed to copy encoder parameters to output stream #%u\n", video_stream_index);
            return ret;
        }

        out_stream->time_base = enc_ctx->time_base;
        out_stream->r_frame_rate = enc_ctx->framerate;
        out_stream->codec->time_base = enc_ctx->time_base;
        enc_ctxs[j] = enc_ctx;

        av_dump_format(ofmt_ctxs[j], 0, filename, 1);

        if (!(ofmt_ctxs[j]->oformat->flags & AVFMT_NOFILE)) {
            ret = avio_open(&ofmt_ctxs[j]->pb, filename, AVIO_FLAG_WRITE);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "Could not open output file '%s'", filename);
                return ret;
            }
        }

        /* init muxer, write output file header */
        ret = avformat_write_header(ofmt_ctxs[j], NULL);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Error occurred when opening output file\n");
            return ret;
        }
    }
    return 0;
}

static int init_filter(FilteringContext* fctx, AVCodecContext *dec_ctx,
                       AVCodecContext *enc_ctx, const char *filter_spec) {
    char args[512];
    int ret = 0;
    const AVFilter *buffersrc = NULL;
    const AVFilter *buffersink = NULL;
    AVFilterContext *buffersrc_ctx = NULL;
    AVFilterContext *buffersink_ctx = NULL;
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs = avfilter_inout_alloc();
    AVFilterGraph *filter_graph = avfilter_graph_alloc();

    if (!outputs || !inputs || !filter_graph) {
        ret = AVERROR(ENOMEM);
        goto end;
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
                             (uint8_t * ) & enc_ctx->pix_fmt, sizeof(enc_ctx->pix_fmt),
                             AV_OPT_SEARCH_CHILDREN);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot set output pixel format\n");
            goto end;
        }
    } else {
        ret = AVERROR_UNKNOWN;
        goto end;
    }

    /* Endpoints for the filter graph. */
    outputs->name = av_strdup("in");
    outputs->filter_ctx = buffersrc_ctx;
    outputs->pad_idx = 0;
    outputs->next = NULL;

    inputs->name = av_strdup("out");
    inputs->filter_ctx = buffersink_ctx;
    inputs->pad_idx = 0;
    inputs->next = NULL;

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

static int init_filters(void) {
    const char *filter_spec;
    int ret;
    int id;

    filter_ctxs = av_malloc_array(nb_multi_output, sizeof(*filter_ctxs));
    if (!filter_ctxs)
        return AVERROR(ENOMEM);

    for (id = 0; id < nb_multi_output; id++) {
        filter_ctxs[id].buffersrc_ctx = NULL;
        filter_ctxs[id].buffersink_ctx = NULL;
        filter_ctxs[id].filter_graph = NULL;
        filter_spec = opts_ctxs[id].filter_desc;

        ret = init_filter(&filter_ctxs[id], dec_ctx, enc_ctxs[id], filter_spec);
        if (ret)
            return ret;
    }

    return 0;
}

static int init_threads(void) {
    int ret = 0;

    pthread_mutex_init(&process_mutex, NULL);
    pthread_cond_init(&process_cond, NULL);
    t_end = 0;
    t_error = 0;

    prod_ctx = (ProducerContext *) malloc(sizeof(ProducerContext));
    if (prod_ctx == NULL) {
        av_log(NULL, AV_LOG_ERROR, "producer_ctx malloc failed\n");
        return -1;
    }
    prod_ctx->input_frm = av_frame_alloc();
    if (!prod_ctx->input_frm) {
        av_log(NULL, AV_LOG_ERROR, "producer_ctx->input_frm malloc failed\n");
        ret = AVERROR(ENOMEM);
        return ret;
    }
    prod_ctx->f_thread = 0;
    prod_ctx->prod_tail = 0;

    cons_ctxs = (ConsumerContext *) malloc(nb_multi_output * sizeof(ConsumerContext));
    if (cons_ctxs == NULL) {
        av_log(NULL, AV_LOG_ERROR, "cons_ctxs malloc failed\n");
        return -1;
    }

    for (int id = 0; id < nb_multi_output; id++) {
        cons_ctxs[id].decoded_frame = av_frame_alloc();
        if (!cons_ctxs[id].decoded_frame) {
            av_log(NULL, AV_LOG_ERROR, "cons_ctxs[i].decoded_frame failed\n");
            ret = AVERROR(ENOMEM);
            return ret;
        }
        cons_ctxs[id].filtered_frame = av_frame_alloc();
        if (!cons_ctxs[id].filtered_frame) {
            av_log(NULL, AV_LOG_ERROR, "cons_ctxs[i].filtered_frame failed\n");
            ret = AVERROR(ENOMEM);
            return ret;
        }
        cons_ctxs[id].f_thread = 0;
        cons_ctxs[id].cons_head = 0;
    }
    return ret;
}

static void *producer_pipeline(void *arg) {
    return;
}

static void *consumer_pipeline(void *arg) {
    return;
}

int main(int argc, char **argv) {
    int ret = 0;

    if (strcmp(argv[1], "-help") == 0) {
        av_log(NULL, AV_LOG_ERROR, "Usage: %s -i <input file> \ \n "
                                   "-nb_outputs 4 \ \n"
                                   "-c:v libsvt_hevc -vf scale=7680:3840 -rc 1 -b:v 13M -g 60 -tune 1 -preset 6 -o o1.mp4 \ \n"
                                   "-c:v libsvt_hevc -vf scale=1920:1080 -rc 1 -b:v 4.5M -g 60 -tune 1 -preset 6 -o o2.mp4 \ \n"
                                   "-c:v libsvt_hevc -vf scale=1280:720 -rc 1 -b:v 2M   -g 60 -tune 1 -preset 6 -o o3.mp4 \ \n"
                                   "-c:v libsvt_hevc -vf scale=480:360  -rc 1 -b:v 0.5M -g 60 -tune 1 -preset 6 -o o4.mp4 \ \n",
               argv[0]);
        return -1;
    }

    if ((ret = parse_options(argc, argv)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "parse_options failed\n");
        goto end;
    }

    if ((ret = open_input_file()) < 0) {
        av_log(NULL, AV_LOG_ERROR, "initialize input file failed\n");
        goto end;
    }

    if ((ret = init_output_file()) < 0)
        goto end;

    if ((ret = init_filters()) < 0)
        goto end;

    if ((ret = init_threads() < 0))
        goto end;

    if((ret = pthread_create(&prod_ctx->f_thread, NULL, producer_pipeline, NULL))) {
        av_log(NULL, AV_LOG_ERROR, "prod_ctx pthread_create failed: %s.\n", strerror(ret));
        goto end;
    }

    for (int id = 0; id < nb_multi_output; id++) {
        if ((ret = pthread_create(&cons_ctxs[id].f_thread, NULL, consumer_pipeline, &filter_ctxs[id]))) {
            av_log(NULL, AV_LOG_ERROR, "cons_ctxs pthread_create failed: %s.\n", strerror(ret));
            goto end;
        }
    }

    pthread_join(prod_ctx->f_thread, NULL);
    for(int id = 0; id<nb_multi_output;id++) {
        pthread_join(cons_ctxs[id].f_thread, NULL);
    }

    end:
    //free threads context
    if (prod_ctx->input_frm) {
        av_frame_free(&prod_ctx->input_frm);
    }

    for (int id = 0; id < nb_multi_output; id++) {
        if (cons_ctxs[id].decoded_frame) {
            av_frame_free(&cons_ctxs[id].decoded_frame);
        }
        if (cons_ctxs[id].filtered_frame) {
            av_frame_free(&cons_ctxs[id].filtered_frame);
        }
    }

    if(cons_ctxs){
        free(cons_ctxs);
    }

    //free filter context
    for (int id = 0; id < nb_multi_output; id++) {
        if (filter_ctxs[id].filter_graph) {
            avfilter_graph_free(&filter_ctxs[id].filter_graph);
        }
    }

    if (filter_ctxs)
        av_free(filter_ctxs);

    //free output context
    for (int id = 0; id < nb_multi_output; id++) {
        if (enc_ctxs[id]) {
            avcodec_free_context(&enc_ctxs[id]);
        }
    }

    if (enc_ctxs) {
        av_free(enc_ctxs);
    }
    for (int j = 0; j < nb_multi_output; j++) {
        if (ofmt_ctxs[j] && !(ofmt_ctxs[j]->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&ofmt_ctxs[j]->pb);
        }
        if (ofmt_ctxs[j]) {
            avformat_free_context(ofmt_ctxs[j]);
        }
    }
    if (ofmt_ctxs) {
        free(ofmt_ctxs);
    }

    //free input context
    if (dec_ctx) {
        avcodec_free_context(&dec_ctx);
    }

    if (ifmt_ctx) {
        avformat_close_input(&ifmt_ctx);
    }

    //free parse option
    if (opts_ctxs) {
        free(opts_ctxs);
    }
    return 0;
}