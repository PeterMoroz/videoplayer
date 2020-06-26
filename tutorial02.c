/****************************************************************************************
 tutorial02.c
 The code based on the tutorial 'How to Write a Video Player in Less Than 1000 Lines'
 http://dranger.com/ffmpeg/

 This is a simple video player that will stream through every video frame as fast as it can.

 Use Makefile to build (assuming that ffmpeg and SDL2 libraries are correctly installed).

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

#include <SDL2/SDL.h>


#define WINDOW_ORIG_X 100
#define WINDOW_ORIG_Y 100
#define WINDOW_WIDTH 1024
#define WINDOW_HEIGHT 768


#include <stdio.h>


/* #define RGB_OUTPUT */
#define YUV_OUTPUT

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
  
#if defined RGB_OUTPUT
  uint8_t *rgb_pict_data[3] = {NULL};
  int rgb_pict_linesize[3] = { 0 };
#elif defined YUV_OUTPUT
  uint8_t *yuv_plane[3] = {NULL};
  int yuv_pitch[3] = { 0 };
#else
#error "Unknown output format"
#endif
  
  SDL_Window* window = NULL;
  SDL_Renderer* renderer = NULL;
  SDL_Texture* texture = NULL;
  
  SDL_Event event;
  SDL_Rect rect;

  if(argc < 2) 
  {
    printf("Usage: %s <movie-file>\n", argv[0]);
    return -1;
  }
  
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0)
  {
    fprintf(stderr, "SDL_Init() error: %s\n", SDL_GetError());
    goto end;
  }
      

  /* Open the media container and read header. */
  if(avformat_open_input(&format_ctx, argv[1], NULL, NULL)!=0) 
  {
    fprintf(stderr, "Could not open file %s\n", argv[1]);
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

 
  /* create SDL window and renderer */
  window = SDL_CreateWindow("avplayer", WINDOW_ORIG_X, WINDOW_ORIG_Y, 
                            video_dec_ctx->width, video_dec_ctx->height, SDL_WINDOW_SHOWN);
  if (!window)
  {
    fprintf(stderr, "SDL_CreateWindow() error: %s\n", SDL_GetError());
    goto end;
  }
  
  renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!renderer)
  {
    fprintf(stderr, "SDL_CreateRenderer() error: %s\n", SDL_GetError());
    goto end;
  }
  
  
#if defined RGB_OUTPUT
  texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, 
                              video_dec_ctx->width, video_dec_ctx->height);
  if (!texture)
  {
    fprintf(stderr, "SDL_CreateTexture() error: %s\n", SDL_GetError());
    goto end;
  }
#elif defined YUV_OUTPUT
/* SDL_PIXELFORMAT_IYUV */
  texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_YV12, SDL_TEXTUREACCESS_STREAMING, 
                              video_dec_ctx->width, video_dec_ctx->height);
  if (!texture)
  {
    fprintf(stderr, "SDL_CreateTexture() error: %s\n", SDL_GetError());
    goto end;
  }
#else
#error "Unknown output format"
#endif
    
#if defined RGB_OUTPUT
  /* initialize SWS context for software scaling */
  sws_ctx = sws_getContext(video_dec_ctx->width, video_dec_ctx->height, video_dec_ctx->pix_fmt,
			                     video_dec_ctx->width, video_dec_ctx->height, AV_PIX_FMT_RGB24,
			                     SWS_BILINEAR, NULL, NULL, NULL);
#elif defined YUV_OUTPUT
  /* initialize SWS context for software scaling */
  sws_ctx = sws_getContext(video_dec_ctx->width, video_dec_ctx->height, video_dec_ctx->pix_fmt,
			                     video_dec_ctx->width, video_dec_ctx->height, AV_PIX_FMT_YUV420P,
			                     SWS_BILINEAR, NULL, NULL, NULL);
#else
#error "Unknown output format"
#endif
  
  if (!sws_ctx)
  {
    fprintf(stderr, "Could not create scale context!\n");
    goto end;
  }
  
  
  frame = av_frame_alloc();
  if (!frame)
  {
    fprintf(stderr, "Could not allocate frame!\n");
    goto end;
  }

#if defined RGB_OUTPUT
  /* allocate raw image with the same dimension as videoframe but with RGB24 layout */
  ret = av_image_alloc(rgb_pict_data, rgb_pict_linesize, video_dec_ctx->width, video_dec_ctx->height, AV_PIX_FMT_RGB24, 1);
  if (ret < 0) 
  {
      fprintf(stderr, "Could not allocate raw image buffer!\n");
      goto end;
  }
#elif defined YUV_OUTPUT
  /* allocate raw image with the same dimension as videoframe but with YUV420 layout */
  ret = av_image_alloc(yuv_plane, yuv_pitch, video_dec_ctx->width, video_dec_ctx->height, AV_PIX_FMT_YUV420P, 1);
  if (ret < 0)
  {
      fprintf(stderr, "Could not allocate raw image buffer!\n");
      goto end;
  }
#else
#error "Unknown output format"
#endif
  
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
        goto end;
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

  #if defined RGB_OUTPUT
        if (SDL_LockTexture(texture, NULL, (void **)rgb_pict_data, rgb_pict_linesize) != 0)
        {
          fprintf(stderr, "SDL_LockTexture() error: %s\n", SDL_GetError());
          goto end;
        }

        /* Convert the image from its native format to RGB */
        sws_scale(sws_ctx, (uint8_t const * const *)frame->data, 
                  frame->linesize, 0, video_dec_ctx->height, rgb_pict_data, rgb_pict_linesize);

        
        SDL_UnlockTexture(texture);
  #elif defined YUV_OUTPUT
        /* Convert the image from its native format to YUV */
        sws_scale(sws_ctx, (uint8_t const * const *)frame->data, 
                  frame->linesize, 0, video_dec_ctx->height, yuv_plane, yuv_pitch);
        
        SDL_UpdateYUVTexture(texture, NULL, 
                            yuv_plane[0], yuv_pitch[0],
                            yuv_plane[1], yuv_pitch[1],
                            yuv_plane[2], yuv_pitch[2]);
        
  #else
  #error "Unknown output format"
  #endif
              
        if (SDL_RenderClear(renderer) != 0)
        {
          fprintf(stderr, "SDL_RenderClear() error: %s\n", SDL_GetError());
          goto end;
        }
        
        rect.x = 0;
        rect.y = 0;
        rect.w = video_dec_ctx->width;
        rect.h = video_dec_ctx->height;
        if (SDL_RenderCopy(renderer, texture, NULL, &rect) != 0)
        {
          fprintf(stderr, "SDL_RenderCopy() error: %s\n", SDL_GetError());
          goto end;
        }
        SDL_RenderPresent(renderer);
        
        /* 40 ms delay means 25 FPS, 
         * TO DO: get actual FPS from stream, and calculate delay 
         */
        SDL_Delay(40);        
      }
    }
    av_packet_unref(&packet);
    
    SDL_PollEvent(&event);
    if (event.type == SDL_QUIT)
      break;
  }

  
end:

  if (video_dec_ctx != NULL)
    avcodec_free_context(&video_dec_ctx);
  
  if (format_ctx != NULL)
    avformat_close_input(&format_ctx);
  
  if (frame != NULL)
    av_frame_free(&frame);
  
  sws_freeContext(sws_ctx);
#if defined RGB_OUTPUT  
  av_free(rgb_pict_data[0]);
#elif defined YUV_OUTPUT
  av_free(yuv_plane[0]);
#else
#error "Unknown output format"
#endif  

  if (texture != NULL)
    SDL_DestroyTexture(texture);
  
  if (renderer != NULL)
    SDL_DestroyRenderer(renderer);
  
  if (window != NULL)
    SDL_DestroyWindow(window);
  
  SDL_Quit();
  
  return 0;
}
