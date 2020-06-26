/****************************************************************************************
 tutorial03.c
 The code based on the tutorial 'How to Write a Video Player in Less Than 1000 Lines'
 http://dranger.com/ffmpeg/

 This is a simple video player that will stream through every video frame as fast as it can
 and play audio (out of sync).

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
#include <libswresample/swresample.h>
#include <libavutil/opt.h>

#include <SDL2/SDL.h>

#define WINDOW_ORIG_X 100
#define WINDOW_ORIG_Y 100
#define WINDOW_WIDTH 1024
#define WINDOW_HEIGHT 768

#define SAMPLE_RATE 48000
#define CHANNELS_NUMBER 2

#define BYTES_PER_SAMPLE 2
#define NUM_OF_SAMPLES 2048

#define AUDIO_BUFFER_SIZE 65536

#include <assert.h>
#include <stdio.h>
#include <stdbool.h>


static int finished = 0;

typedef struct PacketQueue
{
  AVPacketList* first_packet;
  AVPacketList* last_packet;

  int num_of_packets;
  int num_of_bytes;

  SDL_mutex* mutex;
  SDL_cond* cond;
} PacketQueue;

typedef struct AudioContext
{
  AVCodecContext* audio_dec_ctx;
  struct SwrContext *swr_ctx;
  AVFrame* audio_frame;
} AudioContext;


static PacketQueue audio_queue;
static AudioContext audio_context;


static bool packet_queue_init(PacketQueue* queue);
static bool packet_queue_put(PacketQueue* queue, const AVPacket* packet);
static int packet_queue_get(PacketQueue* queue, AVPacket* packet, bool block);
static void packet_queue_destroy(PacketQueue* queue);

static int decode_audio(void* userdata, uint8_t* audio_buffer, int buffer_size);

static void audio_callback(void* userdata, Uint8* stream, int length);


int main(int argc, char *argv[]) 
{
  AVFormatContext* format_ctx = NULL;
  int video_stream_idx = -1, audio_stream_idx = -1;
  AVStream* video_stream = NULL;
  AVStream* audio_stream = NULL;
  AVCodec* video_dec = NULL;
  AVCodec* audio_dec = NULL;
  AVCodecContext* video_dec_ctx = NULL;
  AVCodecContext* audio_dec_ctx = NULL;
  AVPacket packet;
  AVFrame* video_frame = NULL;
  AVFrame* audio_frame = NULL;
  struct SwsContext *sws_ctx = NULL;
  struct SwrContext *swr_ctx = NULL;
  SDL_AudioSpec audio_spec_want, audio_spec;
  SDL_AudioDeviceID audio_device_id = 0;
  int ret = 0;

  uint8_t* rgb_pixels[3] = {NULL};
  int rgb_pitch[3] = {0};
  
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
  
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS | SDL_INIT_TIMER) != 0)
  {
    fprintf(stderr, "SDL_Init() error: %s\n", SDL_GetError());
    goto end;
  }
        
  /* Open the media container and read header. */
  if(avformat_open_input(&format_ctx, argv[1], NULL, NULL) != 0) 
  {
    fprintf(stderr, "Could not open file %s\n", argv[1]);
    goto end;    
  }
    
  /* Retrieve stream information. */
  if(avformat_find_stream_info(format_ctx, NULL) < 0) 
  {
    fprintf(stderr, "Could not find stream information\n");
    goto end;
  }
  
  /* Dump format's information onto standard error. */
  av_dump_format(format_ctx, 0, argv[1], 0);
    
  /* Find the best video stream. */
  video_stream_idx = av_find_best_stream(format_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
  if (video_stream_idx < 0)
  {
    fprintf(stderr, "Could not find video stream in the media container!\n");
    goto end;
  }
  
  /* Find the best audio stream */
  audio_stream_idx = av_find_best_stream(format_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
  if (audio_stream_idx < 0)
  {
    fprintf(stderr, "Could not find audio stream in the media container!\n");
    goto end;
  }

  printf("video stream index: %d\n", video_stream_idx);
  printf("audio stream index: %d\n", audio_stream_idx);

  video_stream = format_ctx->streams[video_stream_idx];
  audio_stream = format_ctx->streams[audio_stream_idx];  

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
    fprintf(stderr, "Could not copy video codec parameters to decoder's context!\n");
    goto end;
  }

  /* Open codec */
  if (avcodec_open2(video_dec_ctx, video_dec, NULL) < 0) 
  {
    fprintf(stderr, "Could not open video codec!\n");
    goto end;
  }
    
  /* Find the decoder for the audio stream */  
  audio_dec = avcodec_find_decoder(audio_stream->codecpar->codec_id);
  if (!audio_dec) 
  {
    fprintf(stderr, "Could not find audio codec!\n");
    goto end;
  }
  
  /* Allocate a codec context for the decoder */
  audio_dec_ctx = avcodec_alloc_context3(audio_dec);
  if (!audio_dec_ctx)
  {
    fprintf(stderr, "Could not allocate audio codec's context!\n");
    goto end;
  }

  /* Copy codec parameters from input stream to output codec context */
  if (avcodec_parameters_to_context(audio_dec_ctx, audio_stream->codecpar) < 0) 
  {
    fprintf(stderr, "Could not copy audio codec parameters to decoder context!\n");
    goto end;
  }

  /* Open codec */
  if (avcodec_open2(audio_dec_ctx, audio_dec, NULL) < 0) 
  {
    fprintf(stderr, "Could not open audio codec!\n");
    goto end;
  }

 
  /* create SDL window and renderer */
  window = SDL_CreateWindow("avplayer", WINDOW_ORIG_X, WINDOW_ORIG_Y, 
                            WINDOW_WIDTH, WINDOW_HEIGHT, SDL_WINDOW_SHOWN);
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
  
  texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, WINDOW_WIDTH, WINDOW_HEIGHT);  
  if (!texture)
  {
    fprintf(stderr, "SDL_CreateTexture() error: %s\n", SDL_GetError());
    goto end;
  }


  sws_ctx = sws_getContext(video_dec_ctx->width, video_dec_ctx->height, video_dec_ctx->pix_fmt,
                           WINDOW_WIDTH, WINDOW_HEIGHT, AV_PIX_FMT_RGB24, SWS_BILINEAR, NULL, NULL, NULL);
  
  
  if (!sws_ctx)
  {
    fprintf(stderr, "Could not create scale context!\n");
    goto end;
  }
  
  
  video_frame = av_frame_alloc();
  if (!video_frame)
  {
    fprintf(stderr, "Could not allocate frame!\n");
    goto end;
  }
  
  audio_frame = av_frame_alloc();
  if (!audio_frame)
  {
    fprintf(stderr, "Could not allocate frame!\n");
    goto end;
  }  
  

  swr_ctx = swr_alloc();
  if (!swr_ctx)
  {
    fprintf(stderr, "Couldn't allocate resample context!\n");
    goto end;
  }

  
  if (!packet_queue_init(&audio_queue))
  {
    fprintf(stderr, "Couldn't initialize audio queue!\n");
    goto end;
  }  
  
  SDL_memset(&audio_spec_want, 0, sizeof(audio_spec_want));

  audio_spec_want.freq = SAMPLE_RATE;
  audio_spec_want.format = AUDIO_S16SYS;
  audio_spec_want.channels = CHANNELS_NUMBER;
  audio_spec_want.silence = 0;
  audio_spec_want.samples = NUM_OF_SAMPLES;
  audio_spec_want.callback = audio_callback;
  audio_spec_want.userdata = NULL;
    
  
  audio_device_id = SDL_OpenAudioDevice(NULL, 0, &audio_spec_want, &audio_spec, 0);
  if (audio_device_id == 0)
  {
    fprintf(stderr, "SDL_OpenAudioDevice() error: %s\n", SDL_GetError());
    goto end;
  }
  
  av_opt_set_int(swr_ctx, "in_channel_count", audio_dec_ctx->channels, 0);
  av_opt_set_int(swr_ctx, "out_channel_count", audio_spec.channels, 0);
  av_opt_set_int(swr_ctx, "in_sample_fmt", audio_dec_ctx->sample_fmt, 0);
  av_opt_set_int(swr_ctx, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
  av_opt_set_int(swr_ctx, "in_channel_layout", audio_dec_ctx->channel_layout, 0);
  av_opt_set_int(swr_ctx, "out_channel_layout", AV_CH_LAYOUT_STEREO, 0);
  av_opt_set_int(swr_ctx, "in_sample_rate", audio_dec_ctx->sample_rate, 0);
  av_opt_set_int(swr_ctx, "out_sample_rate", audio_spec.freq, 0);
  
  swr_init(swr_ctx);

  audio_context.audio_dec_ctx = audio_dec_ctx;
  audio_context.swr_ctx = swr_ctx;
  audio_context.audio_frame = audio_frame;

  SDL_PauseAudioDevice(audio_device_id, 0);
  
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
        fprintf(stderr, "Error sending packet (%s)\n", av_err2str(ret));
        goto end;
      }
            
      while (ret >= 0)
      {
        ret = avcodec_receive_frame(video_dec_ctx, video_frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
          break;
        else if (ret < 0)
        {
          fprintf(stderr, "Error while receiving frame from the decoder (%s)\n", av_err2str(ret));
          break;          
        }


        if (SDL_LockTexture(texture, NULL, (void**)rgb_pixels, rgb_pitch) < 0)
        {
          fprintf(stderr, "SDL_LockTexture() failed: %s\n", SDL_GetError());
        }

        sws_scale(sws_ctx, (uint8_t const * const *)video_frame->data, 
                  video_frame->linesize, 0, video_frame->height, rgb_pixels, rgb_pitch);

        SDL_UnlockTexture(texture);
                  
        if (SDL_RenderClear(renderer) != 0)
        {
          fprintf(stderr, "SDL_RenderClear() error: %s\n", SDL_GetError());
          goto end;
        }

       
        rect.x = 0;
        rect.y = 0;
        rect.w = WINDOW_WIDTH;
        rect.h = WINDOW_HEIGHT;
        if (SDL_RenderCopy(renderer, texture, NULL, &rect) != 0)
        {
          fprintf(stderr, "SDL_RenderCopy() error: %s\n", SDL_GetError());
          goto end;
        }
        SDL_RenderPresent(renderer);
        
        /* 40 ms delay means 25 FPS, 
        */
        SDL_Delay(40);
      }
    }
    else if (packet.stream_index == audio_stream_idx)
    {
      if (!packet_queue_put(&audio_queue, &packet))
      {
        fprintf(stderr, "Could not enqueue audio packet!\n");
        av_packet_unref(&packet);
      }
    }

    if (packet.stream_index != audio_stream_idx)
    {
      av_packet_unref(&packet);
    }
    
    SDL_PollEvent(&event);
    if (event.type == SDL_QUIT)
      break;
  }
  
  finished = 1;
  printf("finished\n");

  printf("close SDL audio device.\n");
  if (audio_device_id != 0)
    SDL_CloseAudioDevice(audio_device_id);

  printf("destroy SDL texture.\n");
  if (texture != NULL)
    SDL_DestroyTexture(texture);
  
  printf("destroy SDL renderer.\n");
  if (renderer != NULL)
    SDL_DestroyRenderer(renderer);
  
  printf("destroy SDL window.\n");
  if (window != NULL)
    SDL_DestroyWindow(window);  

  SDL_Quit();
  
end:

  printf("destroy packets queue.\n");
  packet_queue_destroy(&audio_queue);

  printf("free decoder (video) context.\n");
  if (video_dec_ctx != NULL)
    avcodec_free_context(&video_dec_ctx);
  
  printf("free decoder (audio) context.\n");
  if (audio_dec_ctx != NULL)
    avcodec_free_context(&audio_dec_ctx);  
  
  printf("close format context.\n");
  if (format_ctx != NULL)
    avformat_close_input(&format_ctx);
  
  printf("release video frame.\n");
  if (video_frame != NULL)
    av_frame_free(&video_frame);
  
  printf("release audio frame.\n");
  if (audio_frame != NULL)
    av_frame_free(&audio_frame);
  
  printf("release scale context.\n");
  sws_freeContext(sws_ctx);

  printf("release resample context.\n");
  if (swr_ctx != NULL)
    swr_free(&swr_ctx);

  
  return 0;
}

static bool packet_queue_init(PacketQueue* queue)
{
  assert(queue != NULL);

  queue->first_packet = NULL;
  queue->last_packet = NULL;
  queue->num_of_packets = 0;
  queue->num_of_bytes = 0;

  queue->mutex = SDL_CreateMutex();
  if (!queue->mutex)
  {
    fprintf(stderr, "SDL_CreateMutex() error: %s\n", SDL_GetError());
    return false;
  }
  queue->cond = SDL_CreateCond();
  if (!queue->cond)
  {
    fprintf(stderr, "SDL_CreateCond() error: %s\n", SDL_GetError());
    SDL_DestroyMutex(queue->mutex);
    return false;
  }
  return true;
}

static bool packet_queue_put(PacketQueue* queue, const AVPacket* packet)
{
  assert(queue != NULL);

  AVPacketList* pktl = av_malloc(sizeof(AVPacketList));
  if (!pktl)
  {
    fprintf(stderr, "Could not callocate memory to AVPacketList!\n");
    return false;
  }

  pktl->next = NULL;  

  if (SDL_LockMutex(queue->mutex) != 0)
  {
    fprintf(stderr, "SDL_LockMutex() error: %s\n", SDL_GetError());
    av_free(pktl);
    return false;
  }

  pktl->pkt = *packet;

  if (!queue->last_packet)
    queue->first_packet =  pktl;
  else
    queue->last_packet->next = pktl;

  queue->last_packet = pktl;
  queue->num_of_packets++;
  queue->num_of_bytes += pktl->pkt.size;

  if (SDL_CondSignal(queue->cond) != 0)
  {
    fprintf(stderr, "SDL_CondSignal() error: %s\n", SDL_GetError());
    return false;
  }
  
  if (SDL_UnlockMutex(queue->mutex) != 0)
  {
    fprintf(stderr, "SDL_UnlockMutex() error: %s\n", SDL_GetError());
    return false;
  }

  return true;
}

static int packet_queue_get(PacketQueue* queue, AVPacket* packet, bool block)
{
  assert(queue != NULL);

  if (SDL_LockMutex(queue->mutex) != 0)
  {
    fprintf(stderr, "SDL_LockMutex() error: %s\n", SDL_GetError());
    return -1;
  }

  AVPacketList* pktl = NULL;
  int ret = 0;
  for (;;)
  {
    if (finished)
    {
      break;
    }

    pktl = queue->first_packet;
    if (pktl)
    {
      queue->first_packet = pktl->next;
      if (!queue->first_packet)
        queue->last_packet = NULL;
      queue->num_of_packets--;
      queue->num_of_bytes -= pktl->pkt.size;
      *packet = pktl->pkt;
      av_free(pktl);
      ret = 1;
      break;
    }
    else if (!block)
    {
      break;
    }
    else
    {
      if (SDL_CondWait(queue->cond, queue->mutex) != 0)
      {
        fprintf(stderr, "SDL_CondWait() error: %s\n", SDL_GetError());
        ret = -1;
        break;
      }      
    }
  }

  if (SDL_UnlockMutex(queue->mutex) != 0)
  {
    fprintf(stderr, "SDL_UnlockMutex() error: %s\n", SDL_GetError());
    ret = -1;
  }
  return ret;
}

static void packet_queue_destroy(PacketQueue* queue)
{
  assert(queue != NULL);
  if (queue->mutex == NULL || queue->cond == NULL)
    return;

  fprintf(stderr, "lock queue mutex\n");
  if (SDL_LockMutex(queue->mutex) != 0)
  {
    fprintf(stderr, "SDL_LockMutex() error: %s\n", SDL_GetError());
  }

  AVPacketList* pktl = NULL;
  while ((pktl = queue->first_packet))
  {
    av_packet_unref(&pktl->pkt);
    queue->first_packet = pktl->next;

    if (!queue->first_packet)
      queue->last_packet = NULL;
    queue->num_of_packets--;
    queue->num_of_bytes -= pktl->pkt.size;
    av_free(pktl);
  }  

  fprintf(stderr, "unlock queue mutex\n");
  if (SDL_UnlockMutex(queue->mutex) != 0)
  {
    fprintf(stderr, "SDL_UnlockMutex() error: %s\n", SDL_GetError());
  }
  
  SDL_DestroyMutex(queue->mutex);
  queue->mutex = NULL;
  SDL_DestroyCond(queue->cond);
  queue->cond = NULL;  
}


static void audio_callback(void* userdata, Uint8* stream, int length)
{
  static uint8_t audio_buffer[AUDIO_BUFFER_SIZE];
  static unsigned int audio_data_length = 0;
  static unsigned int audio_buffer_index = 0;

  int decoded_length = 0, audio_chunk_length = 0;

  SDL_memset(stream, 0, length);

  while (length > 0)
  {    
    if (audio_buffer_index >= audio_data_length)
    {
      /* Get more data.
      */
      decoded_length = decode_audio(userdata, audio_buffer, sizeof(audio_buffer));
      if (decoded_length < 0)
      {
        /* Error when decoding, output silence. */
        audio_data_length = 1024;
        memset(audio_buffer, 0, audio_data_length);
      }
      else
      {
        audio_data_length = decoded_length;
      }

      audio_buffer_index = 0;
    }

    audio_chunk_length = audio_data_length - audio_buffer_index;
    if (audio_chunk_length > length)
      audio_chunk_length = length;

    const Uint8* src = &audio_buffer[audio_buffer_index];
    SDL_MixAudioFormat(stream, src, AUDIO_S16SYS, audio_chunk_length, SDL_MIX_MAXVOLUME);

    length -= audio_chunk_length;
    stream += audio_chunk_length;
    audio_buffer_index += audio_chunk_length;
  }

}

static int decode_audio(void* userdata, uint8_t* audio_buffer, int buffer_size)
{
  AVCodecContext* audio_dec_ctx = audio_context.audio_dec_ctx;
  assert(audio_dec_ctx);

  struct SwrContext* swr_ctx = audio_context.swr_ctx;
  assert(swr_ctx);

  AVFrame* audio_frame = audio_context.audio_frame;
  assert(audio_frame);  


  AVPacket audio_packet;
  int ret = packet_queue_get(&audio_queue, &audio_packet, true);

  if (ret == -1)
  {
    fprintf(stderr, "Could not get an audio packet from the queue - an error !\n");
    return -1;
  }
  else if (ret == 0)
  {
    fprintf(stderr, "Could not get an audio packet from the queue - no data !\n");
    return 0;
  }

  ret = avcodec_send_packet(audio_dec_ctx, &audio_packet);
  if (ret < 0)
  {
    fprintf(stderr, "Error sending packet (%s)\n", av_err2str(ret));
    av_packet_unref(&audio_packet);
    return -1;
  }

  int length = 0;

  while (ret >= 0)
  {
    ret = avcodec_receive_frame(audio_dec_ctx, audio_frame);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
      break;
    else if (ret < 0)
    {
      fprintf(stderr, "Error while receiving frame from the decoder (%s)\n", av_err2str(ret));
      break;
    }


    int nsamples_delay = swr_get_delay(swr_ctx, audio_dec_ctx->sample_rate);
    int nsamples = av_rescale_rnd(nsamples_delay + audio_frame->nb_samples, audio_dec_ctx->sample_rate, SAMPLE_RATE, AV_ROUND_UP);

    uint8_t** audio_samples = NULL;
    if (av_samples_alloc_array_and_samples(&audio_samples, NULL, CHANNELS_NUMBER, nsamples, AV_SAMPLE_FMT_S16, 0) < 0)
    {
      fprintf(stderr, "Could not allocate memory to store audiosamples!\n");
      break;
    }

    if (av_samples_alloc(audio_samples, NULL, CHANNELS_NUMBER, nsamples, AV_SAMPLE_FMT_S16, 0) < 0)
    {
      fprintf(stderr, "Could not allocate memory to store audiosamples!\n");
      av_freep(&audio_samples);
      break;
    }

    int nsamples_converted = swr_convert(swr_ctx, audio_samples, nsamples, 
                                        (const uint8_t **)audio_frame->data, audio_frame->nb_samples);

    int nbytes = av_samples_get_buffer_size(NULL, CHANNELS_NUMBER, nsamples_converted, AV_SAMPLE_FMT_S16, 1);

    assert((length + nbytes) <= buffer_size);
    memcpy(audio_buffer, audio_samples[0], nbytes);

    audio_buffer += nbytes;
    length += nbytes;    

    if (audio_samples)
      av_freep(&audio_samples[0]);

    av_freep(&audio_samples);
  }


  av_packet_unref(&audio_packet);

  return length;
}
