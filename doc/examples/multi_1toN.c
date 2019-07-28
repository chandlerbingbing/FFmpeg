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
//config options
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
    return ret;
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

    ret = parse_options(argc, argv);
    if(ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "parse_options failed\n");
        goto end;
    }


    end:
    if(opts_ctxs){
        free(opts_ctxs);
    }

    return ret;
}