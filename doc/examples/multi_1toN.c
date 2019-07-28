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

//input format
static AVFormatContext *ifmt_ctx;
static AVCodecContext **dec_ctxs;


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

static int open_input_file()
{
    int ret;
    unsigned int i;
    const char *filename = input_file;

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

            AVDictionary *dec_opt=NULL;
            //av_dict_set(&dec_opt,"threads","1", 0);
            av_dict_set(&dec_opt, "refcounted_frames", "1", 0);

            ret = avcodec_open2(codec_ctx, dec, &dec_opt);

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

int main(int argc, char **argv) {
    int ret = 0;

    if (strcmp(argv[1], "-help") == 0) {
        av_log(NULL, AV_LOG_ERROR, "Usage: %s -i <input file> \ \n "
                                   "-nb_outputs 4 \ \n"
                                   "-c:v libsvt_hevc -vf scale=7680:3840 -rc 1 -b:v 13M -g 60 -tune 1 -preset 6 -o o1.mp4 \ \n"
                                   "-c:v libsvt_hevc -vf scale=1920:1080 -rc 1 -b:v 4.5M -g 60 -tune 1 -preset 6 -o o2.mp4 \ \n"
                                   "-c:v libsvt_hevc -vf scale=1280:720 -rc 1 -b:v 2M   -g 60 -tune 1 -preset 6 -o o3.mp4 \ \n"
                                   "-c:v libsvt_hevc -vf scale=480:360  -rc 1 -b:v 0.5M -g 60 -tune 1 -preset 6 -o o4.mp4 \ \n", argv[0]);
        return -1;
    }

    if((ret = parse_options(argc, argv)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "parse_options failed\n");
        goto end;
    }

    if ((ret = open_input_file()) < 0){
        av_log(NULL, AV_LOG_ERROR, "initialize input file failed\n");
        goto end;
    }

    end:
    if(opts_ctxs){
        free(opts_ctxs);
    }
    if(ifmt_ctx->nb_streams) {
        for (int i = 0; i < ifmt_ctx->nb_streams; i++) {
            if (dec_ctxs[i])
                avcodec_free_context(&dec_ctxs[i]);
        }
    }
    if(ifmt_ctx) {
        avformat_close_input(&ifmt_ctx);
    }
    return ret;
}