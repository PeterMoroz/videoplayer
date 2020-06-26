/****************************************************************************************
 tutorial01.c
 The code based on the tutorial 'How to Write a Video Player in Less Than 1000 Lines'
 http://dranger.com/ffmpeg/

 This is a sample program that shows how to use ffmpeg libraries to read videostream 
 from a file. 

 Use Makefile to build (assuming that ffmpeg libraries are correctly installed).

 Tested with ffmpeg version N-93276-g3b23eb2, 
 which was built with gcc 7 (Ubuntu 7.3.0-27ubuntu1~18.04).

  libavutil      56. 26.100 / 56. 26.100
  libavcodec     58. 47.102 / 58. 47.102
  libavformat    58. 26.101 / 58. 26.101
  libavdevice    58.  6.101 / 58.  6.101
  libavfilter     7. 48.100 /  7. 48.100
  libswscale      5.  4.100 /  5.  4.100
  libswresample   3.  4.100 /  3.  4.100
  libpostproc    55.  4.100 / 55.  4.100
*/


#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>


#include <stdio.h>

void save_rgb_frame(uint8_t *pix_data[4], int line_size[4], int width, int height, unsigned idx)
{
  FILE* file = NULL;
  char fname[32] = { 0 };
  int y = 0;
  
  /* Open file */
  sprintf(fname, "frame-%04d.ppm", idx);
  file = fopen(fname, "wb");
  
  if(file == NULL)
  {
    fprintf(stderr, "Couldn't open file '%s' to write RGB-frame\n", fname);
    return;
  }
  /* Write header */
  fprintf(file, "P6\n%d %d\n255\n", width, height);
  
  /* Write pixel data */
  for(y = 0; y < height; y++)
    fwrite(pix_data[0] + y * line_size[0], 1, width * 3, file);
  
  /* close file */
  fclose(file);
}



int main(int argc, char *argv[]) 
{
  AVFormatContext* format_ctx = NULL;
  int video_stream_idx = -1, ret = 0;
  AVStream* video_stream = NULL;
  AVCodec* video_dec = NULL;
  AVCodecContext* video_dec_ctx = NULL;
  AVPacket packet;
  AVFrame* frame = NULL;
  struct SwsContext *sws_ctx = NULL;
  
  uint8_t *rgb_pict_data[4] = {NULL};
  int rgb_pict_linesize[4] = { 0 };
  
  unsigned frame_idx = 0;

  if(argc < 2) 
  {
    printf("Please provide path to the file.\n");
    return -1;
  }
  
  /* Read the file header and store information about the file format 
    in the AVFormatContext structure. */
  if(avformat_open_input(&format_ctx, argv[1], NULL, NULL)!=0) 
  {
    fprintf(stderr, "Could not open file '%s'!\n", argv[1]);
    goto end;    
  }
  
  /* Retrieve stream information. */
  if(avformat_find_stream_info(format_ctx, NULL)<0) 
  {
    fprintf(stderr, "Could not find stream information!\n");
    goto end;
  }
  
  /* Dump format's information onto standard error. */
  av_dump_format(format_ctx, 0, argv[1], 0);
    
  /* Find the best video stream */
  video_stream_idx = av_find_best_stream(format_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
  if (video_stream_idx < 0)
  {
    fprintf(stderr, "Could not find video stream in the media container!");
    goto end;
  }

  video_stream = format_ctx->streams[video_stream_idx];
  
  /* Find the decoder for the video stream */  
  video_dec = avcodec_find_decoder(video_stream->codecpar->codec_id);
  if (!video_dec) 
  {
    fprintf(stderr, "Could not find video codec!\n");
    goto end;
  }
  
  /* Allocate a codec context for the decoder */
  video_dec_ctx = avcodec_alloc_context3(video_dec);
  if (!video_dec_ctx)
  {
    fprintf(stderr, "Could not allocate video codec's context!\n");
    goto end;
  }

  /* Copy codec parameters from input stream to output codec context */
  if (avcodec_parameters_to_context(video_dec_ctx, video_stream->codecpar) < 0) 
  {
    fprintf(stderr, "Could not copy video codec's parameters to the decoder context!\n");
    goto end;
  }

  /* Open codec */
  if (avcodec_open2(video_dec_ctx, video_dec, NULL) < 0) 
  {
    fprintf(stderr, "Could not open video codec!\n");
    goto end;
  }
  
  /* initialize SWS context for software scaling */
  sws_ctx = sws_getContext(video_dec_ctx->width, video_dec_ctx->height, video_dec_ctx->pix_fmt,
							video_dec_ctx->width, video_dec_ctx->height, AV_PIX_FMT_RGB24,
							SWS_BILINEAR, NULL, NULL, NULL);
  if (!sws_ctx)
  {
    fprintf(stderr, "Could not create scale context!\n");
    goto end;
  }
  
  /* allocate video frame */
  frame = av_frame_alloc();
  if (!frame)
  {
    fprintf(stderr, "Could not allocate frame!\n");
    goto end;
  }
  
  /* allocate raw image with the same dimension as videoframe but with RGB24 layout */
  ret = av_image_alloc(rgb_pict_data, rgb_pict_linesize, 
  						video_dec_ctx->width, video_dec_ctx->height, AV_PIX_FMT_RGB24, 1);
  if (ret < 0) 
  {
      fprintf(stderr, "Could not allocate raw image buffer!\n");
      goto end;
  }
  
  /* initialize packet, set data to NULL, let the demuxer fill it */
  av_init_packet(&packet);
  packet.data = NULL;
  packet.size = 0;
    
  /* read frames from the file */
  while (av_read_frame(format_ctx, &packet) >= 0)
  {
    if (packet.stream_index == video_stream_idx)
    {
      ret = avcodec_send_packet(video_dec_ctx, &packet);
      if (ret < 0)
      {
		fprintf(stderr, "Error while sending packet to the decoder (%s)\n", av_err2str(ret));
		break;
      }
      
      while (ret >= 0)
      {
	      ret = avcodec_receive_frame(video_dec_ctx, frame);
	      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
	      	break;
	      else if (ret < 0)
	      {
			fprintf(stderr, "Error while receiving frame from the decoder (%s)\n", av_err2str(ret));
			break;
	      }
	      
	      /* Convert the image from its original format to RGB */
	      sws_scale(sws_ctx, (uint8_t const * const *)frame->data, frame->linesize, 0,
								video_dec_ctx->height, rgb_pict_data, rgb_pict_linesize);
	      
	      if (++frame_idx <= 1000)
			save_rgb_frame(rgb_pict_data, rgb_pict_linesize, video_dec_ctx->width, video_dec_ctx->height, frame_idx);
      }

    }
    av_packet_unref(&packet);
  }

  
end:
  if (video_dec_ctx != NULL)
    avcodec_free_context(&video_dec_ctx);
  
  if (format_ctx != NULL)
    avformat_close_input(&format_ctx);
  
  if (frame != NULL)
    av_frame_free(&frame);
  
  sws_freeContext(sws_ctx);
  av_free(rgb_pict_data[0]);
  
  return 0;
}
