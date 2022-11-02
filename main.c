#include <asm-generic/errno-base.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/codec.h>
#include <libavcodec/codec_par.h>
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
#include <stdio.h>

// decode packets into frames
static int decode_packet(AVPacket *pPacket, AVCodecContext *pCodecContext,
                         AVFrame *pFrame);
// save a frame into a .pgm file
static void save_gray_frame(unsigned char *buf, int wrap, int xsize, int ysize,
                            char *filename);

int main(int argc, char *argv[]) {
  if (argc < 2) {
    fprintf(stderr, "media file must be provided.\n");
    return -1;
  }

  fprintf(stdout, "initializing all containers, codecs and protocols.\n");
  // AVFormatContext holds the header information from the format (container)
  AVFormatContext *fmt_ctx = avformat_alloc_context();
  if (!fmt_ctx) {
    fprintf(stderr, "couldn't allocate memory for format context.\n");
    return -1;
  }

  fprintf(stdout,
          "opening input file \"%s\" and loading format (container) header.\n",
          argv[1]);
  if (avformat_open_input(&fmt_ctx, argv[1], NULL, NULL) != 0) {
    fprintf(stderr, "couldn't open file \"%s\"", argv[1]);
    return -1;
  }

  fprintf(stdout, "format %s, duration %ld us, bit rate %ld.\n",
          fmt_ctx->iformat->long_name, fmt_ctx->duration, fmt_ctx->bit_rate);
  fprintf(stdout, "finding stream information from format.\n");
  if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
    fprintf(stderr, "couldn't get stream information.\n");
    return -1;
  }

  // Codec is the component that knows how to enCodec and DECode the video or
  // audio.
  const AVCodec *codec = NULL;
  AVCodecParameters *codec_parameters = NULL;
  // Loop through all the streams and print its main information.
  int video_stream_index = -1;
  for (int i = 0; i < fmt_ctx->nb_streams; i++) {
    AVCodecParameters *local_codec_parameters = fmt_ctx->streams[i]->codecpar;
    fprintf(stdout, "AVStream->time_base before open coded %d/%d.\n",
            fmt_ctx->streams[i]->time_base.num,
            fmt_ctx->streams[i]->time_base.den);
    fprintf(stdout, "AVStream->r_frame_rate before open coded %d/%d.\n",
            fmt_ctx->streams[i]->r_frame_rate.num,
            fmt_ctx->streams[i]->r_frame_rate.den);
    fprintf(stdout, "AVStream->start_time %" PRId64 ".\n",
            fmt_ctx->streams[i]->start_time);
    fprintf(stdout, "AVStream->duration %" PRId64 ".\n",
            fmt_ctx->streams[i]->duration);

    fprintf(stdout, "finding the proper decoder (CODEC).\n");
    // Finds the registered decoder for a codec id.
    const AVCodec *local_codec =
        avcodec_find_decoder(local_codec_parameters->codec_id);
    if (local_codec == NULL) {
      fprintf(stderr, "unsupported codec.\n");
      continue;
    }

    // When the stream is a video we store its index, codec parameters and
    // codec.
    if (local_codec_parameters->codec_type == AVMEDIA_TYPE_VIDEO) {
      if (video_stream_index == -1) {
        codec_parameters = local_codec_parameters;
        video_stream_index = i;
        codec = local_codec;
      }
      fprintf(stdout, "video codec: resolution %d x %d\n",
              local_codec_parameters->width, local_codec_parameters->height);
    } else {
      fprintf(stdout, "audio codec: channels %d, sample rate %d\n",
              local_codec_parameters->ch_layout.nb_channels,
              local_codec_parameters->sample_rate);
    }

    fprintf(stdout, "\tcodec %s id %d bit rate %ld\n", local_codec->name,
            local_codec->id, local_codec_parameters->bit_rate);
  }

  if (video_stream_index == -1) {
    fprintf(stderr, "file %s doesn't contain a video stream.\n", argv[1]);
    return -1;
  }

  AVCodecContext *codec_ctx = avcodec_alloc_context3(codec);
  if (!codec_ctx) {
    fprintf(stderr, "failed to allocate memory for AVCodecContext.\n");
    return -1;
  }

  // Fill the codec context based on the values form the supplied codec
  // parameters.
  if (avcodec_parameters_to_context(codec_ctx, codec_parameters) < 0) {
    fprintf(stderr, "failed to copy codec params to codec context.\n");
    return -1;
  }

  // Initialize the AVCodecContext to use the given AVCodec.
  if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
    fprintf(stderr, "failed to open codec through avcodec_open2.\n");
    return -1;
  }

  AVFrame *frame = av_frame_alloc();
  if (!frame) {
    fprintf(stderr, "failed to allocate memory for AVFrame.\n");
    return -1;
  }

  AVPacket *packet = av_packet_alloc();
  if (!packet) {
    fprintf(stderr, "failed to allocate memory for AVPacket.\n");
    return -1;
  }

  int response = 0;
  int packets_to_process = 8;
  // Fill the packet with data from the stream.
  while (av_read_frame(fmt_ctx, packet) >= 0) {
    // If is the video stream.
    if (packet->stream_index == video_stream_index) {
      fprintf(stdout, "AVPacket->pts %" PRId64 ".\n", packet->pts);
      if (decode_packet(packet, codec_ctx, frame) < 0) {
        // Stop, otherwise we'll be saving hundreds of frames.
        break;
      }

      packets_to_process--;
      if (packets_to_process <= 0) {
        break;
      }
    }
  }

  return 0;
}

static int decode_packet(AVPacket *packet, AVCodecContext *codec_ctx,
                         AVFrame *frame) {
  // Supply raw packet data as input to a decoder.
  int response = avcodec_send_packet(codec_ctx, packet);
  if (response < 0) {
    fprintf(stderr, "error while sending a packet to the decoder: %s.\n",
            av_err2str(response));
    return response;
  }

  while (response >= 0) {
    // Return decoded output data (into a frame) form decoder.
    response = avcodec_receive_frame(codec_ctx, frame);
    if (response == AVERROR(EAGAIN) || response == AVERROR_EOF) {
      return 0;
    }

    if (response < 0) {
      fprintf(stderr, "error while receiving a frame form the decoder: %s",
              av_err2str(response));
      return response;
    }

    fprintf(stdout,
            "frame %d (type=%c, size=%d bytes, format=%d) pts %ld key frame %d "
            "[DTS %d]\n",
            codec_ctx->frame_number, av_get_picture_type_char(frame->pict_type),
            frame->pkt_size, frame->format, frame->pts, frame->key_frame,
            frame->coded_picture_number);
    char frame_filename[1024];
    snprintf(frame_filename, sizeof(frame_filename), "%s-%d.pgm", "frame",
             codec_ctx->frame_number);
    // Check if the frame is a planar YUV 4:2:0, 12bpp
    // That is the format of the provided .mp4 file
    // RGB formats will definitely not give a gray image
    // Other YUV image may do so, but untested, so give a warning
    if (frame->format != AV_PIX_FMT_YUV420P) {
      fprintf(
          stdout,
          "warning: the generated file may not be a grayscale image, but could "
          "e.g. be just the R component if the video format is RGB.\n");
    }

    save_gray_frame(frame->data[0], frame->linesize[0], frame->width,
                    frame->height, frame_filename);
  }

  return 0;
}

static void save_gray_frame(unsigned char *buf, int wrap, int xsize, int ysize,
                            char *filename) {
  FILE *f = fopen(filename, "w");
  // Writing the minimal required header for a pgm file format
  // portable graymap format ->
  // https://en.wikipedia.org/wiki/Netpbm_format#PGM_example.
  fprintf(f, "P5\n%d %d\n%d\n", xsize, ysize, 255);

  // Writing line by line.
  for (int i = 0; i < ysize; i++) {
    fwrite(buf + i * wrap, 1, xsize, f);
  }

  fclose(f);
}
