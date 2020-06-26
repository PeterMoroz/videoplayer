/****************************************************************************************
 tutorial07.c
 The code based on the tutorial 'How to Write a Video Player in Less Than 1000 Lines'
 http://dranger.com/ffmpeg/

 This is a simple video player that plays audio and video streams synchronously.

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
#include <libavutil/time.h>

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
#define AUDIO_DIFF_AVG_NB 20
#define AUDIO_DIFF_THRESHOLD 10
#define SAMPLE_CORRECTION_PERCENT_MAX 10


#define MAX_AUDIOQ_SIZE (5 * 16 * 1024)
#define MAX_VIDEOQ_SIZE (5 * 256 * 1024)

#define SYNC_THRESHOLD 0.01
#define NOSYNC_THRESHOLD 10.0

#define PICTURE_QUEUE_SIZE 1


#define REFRESH_SCREEN_EVENT (SDL_USEREVENT + 1)
#define QUIT_EVENT (SDL_USEREVENT + 2)


#include <assert.h>
#include <stdio.h>
#include <stdbool.h>


typedef enum SyncMasterSource
{
  SyncAudioMaster,
  SyncVideoMaster,
  SyncExternalMaster
} SyncMasterSource;


#define DEFAULT_SYNC_SOURCE SyncVideoMaster

static SyncMasterSource sync_master_source = DEFAULT_SYNC_SOURCE;


static int finished = 0;
static double aspect_ratio = 0.0;

static double audio_clock = 0.0;
static double video_clock = 0.0;

static double frame_last_delay = 0.0;
static double frame_last_pts = 0.0;
static double frame_timer = 0.0;

static double video_current_pts = 0.0;
static int64_t video_current_pts_time = 0;

static uint8_t audio_buffer[AUDIO_BUFFER_SIZE];
static unsigned int audio_data_length = 0;
static unsigned int audio_buffer_index = 0;

static double audio_diff_cum = 0.0;
static double audio_diff_avg_coeff = 0.1;
static unsigned audio_diff_avg_count = 0;

static int seek_req = 0;
static int seek_flags = 0;
static int64_t seek_pos = 0;


typedef struct PacketQueue
{
  AVPacketList* first_packet;
  AVPacketList* last_packet;

  int num_of_packets;
  int num_of_bytes;

  SDL_mutex* mutex;
  SDL_cond* cond;
} PacketQueue;

typedef struct MediaContainer
{
  AVFormatContext* format_ctx;
  int video_stream_idx, audio_stream_idx;
  AVStream* video_stream;
  AVStream* audio_stream;
} MediaContainer;

typedef struct Decoder
{
  AVCodec* codec;
  AVCodecContext* codec_ctx;
} Decoder;

typedef struct AudioDevice
{
  SDL_AudioSpec audio_spec;
  SDL_AudioDeviceID id;
} AudioDevice;

typedef struct AudioResample
{
  AVFrame* frame;
  struct SwrContext *swr_ctx;
} AudioResample;

typedef struct VideoDevice
{
  SDL_Window* window;
  SDL_Renderer* renderer;
} VideoDevice;

typedef struct VideoRescale
{
  struct SwsContext *sws_ctx;
} VideoRescale;

typedef struct Picture
{
  SDL_Texture* texture;
  SDL_mutex* mutex;
  int width;
  int height;
  double pts;
} Picture;

typedef struct PictureQueue
{
  int size;
  int wr_index;
  int rd_index;
  SDL_mutex* mutex;
  SDL_cond* cond;
  Picture pictures[PICTURE_QUEUE_SIZE];
} PictureQueue;



static MediaContainer media_container;

static Decoder audio_decoder;
static Decoder video_decoder;

static PacketQueue audio_queue;
static PacketQueue video_queue;

static AudioDevice audio_device;
static AudioResample audio_resample;

static VideoDevice video_device;
static VideoRescale video_rescale;

static PictureQueue picture_queue;

static SDL_Thread* parse_container_thread = NULL;
static SDL_Thread* decode_video_thread = NULL;


static bool init_media_container(MediaContainer* media_container, const char* filename);
static void release_media_container(MediaContainer* media_container);

static bool init_audio_decoder(Decoder* decoder, AVStream* stream);
static bool init_video_decoder(Decoder* decoder, AVStream* stream);
static void release_decoder(Decoder* decoder);

static bool init_audio_device(AudioDevice* device);
static void release_audio_device(AudioDevice* device);

static bool init_audio_resample(AudioResample* resample, 
                                int in_channel_count, int out_channel_count, 
                                int in_sample_format, int out_sample_format,
                                int in_channel_layout, int out_channel_layout,
                                int in_sample_rate, int out_sample_rate);
static void release_audio_resample(AudioResample* resample);

static bool init_video_device(VideoDevice* device, int xorig, int yorig, int width, int height);
static void release_video_device(VideoDevice* device);

static bool init_video_rescale(VideoRescale* rescale, 
                              int in_width, int in_height, int in_format,
                              int out_width, int out_height, int out_format);
static void release_video_rescale(VideoRescale* rescale);

static bool packet_queue_init(PacketQueue* queue);
static bool packet_queue_put(PacketQueue* queue, const AVPacket* packet);
static int packet_queue_get(PacketQueue* queue, AVPacket* packet, bool block);
static void packet_queue_destroy(PacketQueue* queue);
static void packet_queue_flush(PacketQueue* queue);

static bool picture_queue_init(PictureQueue* queue);
static bool picture_queue_put_frame(PictureQueue* queue, const AVFrame* frame, double pts);
static void picture_queue_peek(PictureQueue* queue, Picture** picture);
static bool picture_queue_consume(PictureQueue* queue);
static void picture_queue_destroy(PictureQueue* queue);


static double get_master_clock(void);

static void picture_display(const Picture* picture);

static int decode_audio(void* userdata, uint8_t* audio_buffer, int buffer_size);
static int synchronize_audio(uint8_t* audio_buffer, int samples_size);

static void audio_callback(void* userdata, Uint8* stream, int length);

static void refresh_screen(void);

static void schedule_screen_refresh(Uint32 delay_ms);

static void synchronize_video(const AVFrame* frame, double pts);


/* Thread functions */
static int parse_container(void* userdata);
static int decode_video(void* userdata);



int main(int argc, char *argv[]) 
{
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

  if (!init_media_container(&media_container, argv[1]))
  {
    fprintf(stderr, "init_media_container() failed!\n");
    goto end;
  }

  /* media_container.video_stream->codecpar->sample_aspect_ratio.num, media_container.video_stream->codecpar->sample_aspect_ratio.den,  */
  if (media_container.video_stream->codecpar->sample_aspect_ratio.num != 0)
  {
    aspect_ratio = av_q2d(media_container.video_stream->codecpar->sample_aspect_ratio) * 
                      media_container.video_stream->codecpar->width / media_container.video_stream->codecpar->height;
  }

  if (aspect_ratio <= 0.0)
  {
    aspect_ratio = (double)media_container.video_stream->codecpar->width / (double)media_container.video_stream->codecpar->height;
  }

  fprintf(stderr, "aspect_ratio: %lf\n", aspect_ratio);

  if (!init_audio_decoder(&audio_decoder, media_container.audio_stream))
  {
    fprintf(stderr, "init_audio_decoder() failed!\n");
    goto end;
  }

  if (!init_video_decoder(&video_decoder, media_container.video_stream))
  {
    fprintf(stderr, "init_video_decoder() failed!\n");
    goto end;
  }


  if (!init_audio_device(&audio_device))
  {
    fprintf(stderr, "init_audio_device() failed!\n");
    goto end;
  }


  if (!init_audio_resample(&audio_resample,                         
                          audio_decoder.codec_ctx->channels, audio_device.audio_spec.channels,
                          audio_decoder.codec_ctx->sample_fmt, AV_SAMPLE_FMT_S16,
                          audio_decoder.codec_ctx->channel_layout, AV_CH_LAYOUT_STEREO,
                          audio_decoder.codec_ctx->sample_rate, audio_device.audio_spec.freq))
  {
    fprintf(stderr, "init_audio_resample() failed!\n");
    goto end;
  }


  if (!init_video_device(&video_device, WINDOW_ORIG_X, WINDOW_ORIG_Y, WINDOW_WIDTH, WINDOW_HEIGHT))
  {
    fprintf(stderr, "init_video_device() failed!\n");
    goto end;
  }


  if (!init_video_rescale(&video_rescale, 
                        video_decoder.codec_ctx->width, video_decoder.codec_ctx->height, video_decoder.codec_ctx->pix_fmt,
                        WINDOW_WIDTH, WINDOW_HEIGHT, AV_PIX_FMT_RGB24))
  {
    fprintf(stderr, "init_video_rescale() failed!\n");
    goto end;
  }


  if (!packet_queue_init(&audio_queue))
  {
    fprintf(stderr, "Couldn't initialize audio queue!\n");
    goto end;
  }

  if (!packet_queue_init(&video_queue))
  {
    fprintf(stderr, "Couldn't initialize video queue!\n");
    goto end;
  }


  if (!picture_queue_init(&picture_queue))
  {
    fprintf(stderr, "Coouln't initialize pictures queue!\n");
    goto end;
  }


  frame_last_delay = 40e-3;
  frame_timer = (double)av_gettime() / 1000000.0;

  video_current_pts_time = av_gettime();

  schedule_screen_refresh(40);

  parse_container_thread = SDL_CreateThread(parse_container, "parse media container", NULL);
  if (!parse_container_thread)
  {
    fprintf(stderr, "SDL_CreateThread() error: %s\n", SDL_GetError());
    goto end;
  }  

  decode_video_thread = SDL_CreateThread(decode_video, "decode wideo", NULL);
  if (!decode_video_thread)
  {
    fprintf(stderr, "SDL_CreateThread() error: %s\n", SDL_GetError());
    goto end;
  }


  SDL_PauseAudioDevice(audio_device.id, 0);  
  
  SDL_Event event;

  for (;;)
  {
    double seek_amount = 0.0;
    double pos = 0.0;

    SDL_WaitEvent(&event);

    switch (event.type)
    {
      case QUIT_EVENT:
      case SDL_QUIT:
        finished = 1;
        break;

      case SDL_KEYDOWN:
        switch (event.key.keysym.sym)
        {
          case SDLK_UP:
            seek_amount = 60.0;
            goto seek_stream;

          case SDLK_RIGHT:
            seek_amount = 10.0;
            goto seek_stream;

          case SDLK_LEFT:
            seek_amount = -10.0;
            goto seek_stream;

          case SDLK_DOWN:
            seek_amount = -60.0;
            goto seek_stream;

          seek_stream:
            pos = get_master_clock();
            pos += seek_amount;
            if (!seek_req)
            {
              seek_pos = pos * AV_TIME_BASE;
              seek_flags = seek_amount < 0 ? AVSEEK_FLAG_BACKWARD : 0;
              seek_req = 1;
            }
            break;

          default:
            break;
        }
        break;

      case REFRESH_SCREEN_EVENT:
        refresh_screen();
        break;

      default:
        break;
    }

    if (finished)
      break;
  }


end:

  SDL_WaitThread(parse_container_thread, NULL);
  parse_container_thread = NULL;

  SDL_WaitThread(decode_video_thread, NULL);
  decode_video_thread = NULL;

  picture_queue_destroy(&picture_queue);
  
  release_video_device(&video_device);
  release_video_rescale(&video_rescale);
  
  /* stop audio callbacks for a second */
  SDL_PauseAudioDevice(audio_device.id, 1);

  release_audio_device(&audio_device);
  release_audio_resample(&audio_resample);

  SDL_Quit();

  packet_queue_destroy(&audio_queue);
  packet_queue_destroy(&video_queue);

  release_decoder(&audio_decoder);
  release_decoder(&video_decoder);

  release_media_container(&media_container);

    
  return 0;
}


static bool init_media_container(MediaContainer* media_container, const char* filename)
{
  assert(media_container != NULL);

  AVFormatContext* format_ctx = NULL;
  int video_stream_idx = -1, audio_stream_idx = -1;
  AVStream* video_stream = NULL;
  AVStream* audio_stream = NULL;

  /* Open the media container and read header. */
  if(avformat_open_input(&format_ctx, filename, NULL, NULL) != 0)
  {
    fprintf(stderr, "Could not open file %s\n", filename);
    return false;
  }

  media_container->format_ctx = format_ctx;
    
  /* Retrieve stream information. */
  if(avformat_find_stream_info(format_ctx, NULL) < 0) 
  {
    fprintf(stderr, "Could not find stream information!\n");
    return false;
  }
  
  /* Dump format's information onto standard error. */
  av_dump_format(format_ctx, 0, filename, 0);
    
  /* Find the best video stream. */
  video_stream_idx = av_find_best_stream(format_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
  if (video_stream_idx < 0)
  {
    fprintf(stderr, "Could not find video stream in the media container!\n");
    return false;
  }

  media_container->video_stream_idx = video_stream_idx;

  
  /* Find the best audio stream */
  audio_stream_idx = av_find_best_stream(format_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
  if (audio_stream_idx < 0)
  {
    fprintf(stderr, "Could not find audio stream in the media container!\n");
    return false;
  }

  media_container->audio_stream_idx = audio_stream_idx;

  printf("video stream index: %d\n", video_stream_idx);
  printf("audio stream index: %d\n", audio_stream_idx);

  video_stream = format_ctx->streams[video_stream_idx];
  audio_stream = format_ctx->streams[audio_stream_idx];

  media_container->video_stream = video_stream;
  media_container->audio_stream = audio_stream;

  return true;
}

static void release_media_container(MediaContainer* media_container)
{
  assert(media_container);

  printf("close format context.\n");
  if (media_container->format_ctx != NULL)
    avformat_close_input(&media_container->format_ctx);

  media_container->format_ctx = NULL;

  media_container->video_stream_idx = -1;
  media_container->audio_stream_idx = -1;

  media_container->video_stream = NULL;
  media_container->audio_stream = NULL;
}


static bool init_audio_decoder(Decoder* decoder, AVStream* stream)
{
  assert(decoder != NULL);
  assert(stream != NULL);

  AVCodec* codec = NULL;
  AVCodecContext* codec_ctx = NULL;
    
  /* Find the decoder for the audio stream */  
  codec = avcodec_find_decoder(stream->codecpar->codec_id);
  if (!codec)
  {
    fprintf(stderr, "Could not find audio codec!\n");
    return false;
  }

  decoder->codec = codec;

  
  /* Allocate a codec context for the decoder */
  codec_ctx = avcodec_alloc_context3(codec);
  if (!codec_ctx)
  {
    fprintf(stderr, "Could not allocate audio codec's context!\n");
    return false;
  }

  decoder->codec_ctx = codec_ctx;


  /* Copy codec parameters from input stream to output codec context */
  if (avcodec_parameters_to_context(codec_ctx, stream->codecpar) < 0) 
  {
    fprintf(stderr, "Could not copy audio codec parameters to decoder context!\n");
    return false;
  }

  /* Open codec */
  if (avcodec_open2(codec_ctx, codec, NULL) < 0)
  {
    fprintf(stderr, "Could not open audio codec!\n");
    return false;
  }

  return true;
}

static bool init_video_decoder(Decoder* decoder, AVStream* stream)
{
  assert(decoder != NULL);
  assert(stream != NULL);

  AVCodec* codec = NULL;
  AVCodecContext* codec_ctx = NULL;


  /* Find the decoder for the video stream */  
  codec = avcodec_find_decoder(stream->codecpar->codec_id);
  if (!codec) 
  {
    fprintf(stderr, "Could not find video codec!\n");
    return false;
  }

  decoder->codec = codec;

  
  /* Allocate a codec context for the decoder */
  codec_ctx = avcodec_alloc_context3(codec);
  if (!codec_ctx)
  {
    fprintf(stderr, "Could not allocate video codec's context!\n");
    return false;
  }

  decoder->codec_ctx = codec_ctx;

  /* Copy codec parameters from input stream to output codec context */
  if (avcodec_parameters_to_context(codec_ctx, stream->codecpar) < 0)
  {
    fprintf(stderr, "Could not copy video codec parameters to decoder's context!\n");
    return false;
  }

  /* Open codec */
  if (avcodec_open2(codec_ctx, codec, NULL) < 0) 
  {
    fprintf(stderr, "Could not open video codec!\n");
    return false;
  }

  return true;  
}

static void release_decoder(Decoder* decoder)
{
  assert(decoder != NULL);

  if (decoder->codec_ctx != NULL)
    avcodec_free_context(&decoder->codec_ctx);

  decoder->codec_ctx = NULL;
  decoder->codec = NULL;
}

static bool init_audio_device(AudioDevice* device)
{
  assert(device != NULL);

  SDL_AudioSpec audio_spec;
  SDL_memset(&audio_spec, 0, sizeof(audio_spec));

  audio_spec.freq = SAMPLE_RATE;
  audio_spec.format = AUDIO_S16SYS;
  audio_spec.channels = CHANNELS_NUMBER;
  audio_spec.silence = 0;
  audio_spec.samples = NUM_OF_SAMPLES;
  audio_spec.callback = audio_callback;
  audio_spec.userdata = NULL;
    
  int audio_device_id = SDL_OpenAudioDevice(NULL, 0, &audio_spec, &device->audio_spec, 0);
  if (audio_device_id == 0)
  {
    fprintf(stderr, "SDL_OpenAudioDevice() error: %s\n", SDL_GetError());
    return false;
  }

  device->id = audio_device_id;

  return true;
}

static void release_audio_device(AudioDevice* device)
{
  assert(device != NULL);

  if (device->id != 0)
    SDL_CloseAudioDevice(device->id);

  SDL_memset(&device->audio_spec, 0, sizeof(device->audio_spec));
  device->id = -1;
}


static bool init_audio_resample(AudioResample* resample, 
                              int in_channel_count, int out_channel_count, 
                              int in_sample_format, int out_sample_format,
                              int in_channel_layout, int out_channel_layout,
                              int in_sample_rate, int out_sample_rate)
{
  assert(resample != NULL);

  AVFrame* frame = av_frame_alloc();
  if (!frame)
  {
    fprintf(stderr, "Could not allocate audio frame!\n");
    return false;
  }

  struct SwrContext *swr_ctx = swr_alloc();
  if (!swr_ctx)
  {
    fprintf(stderr, "Could not allocate resample context!\n");
    av_frame_free(&frame);
    return false;
  }

  av_opt_set_int(swr_ctx, "in_channel_count", in_channel_count, 0);
  av_opt_set_int(swr_ctx, "out_channel_count", out_channel_count, 0);
  av_opt_set_int(swr_ctx, "in_sample_fmt", in_sample_format, 0);
  av_opt_set_int(swr_ctx, "out_sample_fmt", out_sample_format, 0);
  av_opt_set_int(swr_ctx, "in_channel_layout", in_channel_layout, 0);
  av_opt_set_int(swr_ctx, "out_channel_layout", out_channel_layout, 0);
  av_opt_set_int(swr_ctx, "in_sample_rate", in_sample_rate, 0);
  av_opt_set_int(swr_ctx, "out_sample_rate", out_sample_rate, 0);
  
  swr_init(swr_ctx);

  resample->frame = frame;
  resample->swr_ctx = swr_ctx;

  return true;  
}

static void release_audio_resample(AudioResample* resample)
{
  assert(resample != NULL);

  if (resample->frame != NULL)
    av_frame_free(&resample->frame);

  resample->frame = NULL;

  if (resample->swr_ctx != NULL)
    swr_free(&resample->swr_ctx);

  resample->swr_ctx = NULL;
}


static bool init_video_device(VideoDevice* device, int xorig, int yorig, int width, int height)
{
  assert(device != NULL);

  SDL_Window* window = NULL;
  SDL_Renderer* renderer = NULL;
 
  /* create SDL window and renderer */
  window = SDL_CreateWindow("avplayer", xorig, yorig, width, height, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
  if (!window)
  {
    fprintf(stderr, "SDL_CreateWindow() error: %s\n", SDL_GetError());
    return false;
  }
  
  renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!renderer)
  {
    fprintf(stderr, "SDL_CreateRenderer() error: %s\n", SDL_GetError());
    SDL_DestroyWindow(window);
    return false;
  }
  
  device->window = window;
  device->renderer = renderer;

  return true;
}

static void release_video_device(VideoDevice* device)
{
  assert(device != NULL);

  if (device->renderer != NULL)
    SDL_DestroyRenderer(device->renderer);

  device->renderer = NULL;

  
  if (device->window != NULL)
    SDL_DestroyWindow(device->window);

  device->window = NULL;
}


static bool init_video_rescale(VideoRescale* rescale, 
                              int in_width, int in_height, int in_pix_format,
                              int out_width, int out_height, int out_pix_format)
{
  assert(rescale != NULL);


  /* initialize SWS context for software scaling */  
  struct SwsContext *sws_ctx = sws_getContext(in_width, in_height, in_pix_format,
                                              out_width, out_height, out_pix_format,
                                              SWS_BILINEAR, NULL, NULL, NULL);

  if (!sws_ctx)
  {
    fprintf(stderr, "Could not allocate scale context!\n");
    return false;
  }

  rescale->sws_ctx = sws_ctx;

  return true;
}

static void release_video_rescale(VideoRescale* rescale)
{
  assert(rescale != NULL);

  if (rescale->sws_ctx != NULL)
    sws_freeContext(rescale->sws_ctx);

  rescale->sws_ctx = NULL;
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

  if (SDL_UnlockMutex(queue->mutex) != 0)
  {
    fprintf(stderr, "SDL_UnlockMutex() error: %s\n", SDL_GetError());
  }
  
  SDL_DestroyMutex(queue->mutex);
  queue->mutex = NULL;
  SDL_DestroyCond(queue->cond);
  queue->cond = NULL;
}

static void packet_queue_flush(PacketQueue* queue)
{
  assert(queue != NULL);
  assert(queue->mutex != NULL);

  if (SDL_LockMutex(queue->mutex) != 0)
  {
    fprintf(stderr, "SDL_LockMutex() failed: %s\n", SDL_GetError());
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

  assert(queue->last_packet == NULL);
  assert(queue->first_packet == NULL);
  assert(queue->num_of_packets == 0);
  /* assert(queue->num_of_bytes == 0); */
  queue->num_of_bytes = 0;

  if (SDL_UnlockMutex(queue->mutex) != 0)
  {
    fprintf(stderr, "SDL_UnlockMutex() failed: %s\n", SDL_GetError());
  }
}


static bool picture_queue_init(PictureQueue* queue)
{
  assert(queue != NULL);

  queue->size = 0;
  queue->wr_index = 0;
  queue->rd_index = 0;

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

  int i = 0;
  Picture* picture = &queue->pictures[0];
  for(; i < PICTURE_QUEUE_SIZE; i++, picture++)
  {
    picture->texture = NULL;
    picture->width = 0;
    picture->height = 0;
    picture->mutex = SDL_CreateMutex();
    if (!picture->mutex)
    {
      fprintf(stderr, "SDL_CreateMutex() error: %s\n", SDL_GetError());
      while (i-- >= 0)
      {
        picture--;
        if (picture->mutex)
          SDL_DestroyMutex(picture->mutex);
        picture->mutex = NULL;
      }
      return false;
    }
  }

  return true;
}

static bool picture_queue_put_frame(PictureQueue* queue, const AVFrame* frame, double pts)
{
  assert(queue != NULL);

  if (SDL_LockMutex(queue->mutex) != 0)
  {
    fprintf(stderr, "SDL_LockMutex() error: %s\n", SDL_GetError());
    return false;
  }

  while (!finished && queue->size >= PICTURE_QUEUE_SIZE)
  {
    if (SDL_CondWait(queue->cond, queue->mutex) != 0)
    {
      fprintf(stderr, "SDL_CondWait() error: %s\n", SDL_GetError());
      SDL_UnlockMutex(queue->mutex);
      return false;
    }
  }

  if (SDL_UnlockMutex(queue->mutex) != 0)
  {
    fprintf(stderr, "SDL_UnlockMutex() error: %s\n", SDL_GetError());
    return false;
  }

  if (finished)
    return false;

  int width = 0, height = 0;
  SDL_GetWindowSize(video_device.window, &width, &height);

  Picture* picture = &queue->pictures[queue->wr_index];
  if (!picture->texture)
  {
    if (SDL_LockMutex(picture->mutex) != 0)
    {
      fprintf(stderr, "SDL_LockMutex() error: %s\n", SDL_GetError());
      return false;
    }

    picture->texture = SDL_CreateTexture(video_device.renderer, SDL_PIXELFORMAT_RGB24, 
                                        SDL_TEXTUREACCESS_STREAMING, width, height);
    if (!picture->texture)
    {
      fprintf(stderr, "SDL_CreateTexture() error: %s\n", SDL_GetError());
      if (SDL_UnlockMutex(picture->mutex) != 0)
        fprintf(stderr, "SDL_UnlockMutex() error: %s\n", SDL_GetError());
      return false;
    }
    picture->width = width;
    picture->height = height;

    if (SDL_UnlockMutex(picture->mutex) != 0)
    {
      fprintf(stderr, "SDL_UnlockMutex() error: %s\n", SDL_GetError());    
      return false;
    }
  }

  picture->pts = pts;

  uint8_t* rgb_pixels[3] = { NULL };
  int rgb_pitch[3] = { 0 };

  if (SDL_LockTexture(picture->texture, NULL, (void**)rgb_pixels, rgb_pitch) < 0)
  {
    fprintf(stderr, "SDL_LockTexture() failed: %s\n", SDL_GetError());
    return false;
  }


  struct SwsContext *sws_ctx = video_rescale.sws_ctx;
  assert(sws_ctx != NULL);
  assert(frame != NULL);  

  int out_slice_height = sws_scale(sws_ctx, (uint8_t const * const *)frame->data,
                                  frame->linesize, 0, frame->height, rgb_pixels, rgb_pitch);

  SDL_UnlockTexture(picture->texture);

  if (++queue->wr_index == PICTURE_QUEUE_SIZE)
    queue->wr_index = 0;

  if (SDL_LockMutex(queue->mutex) != 0)
  {
    fprintf(stderr, "SDL_LockMutex() error: %s\n", SDL_GetError());
    return false;
  }

  queue->size += 1;

  if (SDL_UnlockMutex(queue->mutex) != 0)
  {
    fprintf(stderr, "SDL_UnlockMutex() error: %s\n", SDL_GetError());
    return false;
  }

  return true;
}

static void picture_queue_peek(PictureQueue* queue, Picture** picture)
{
  assert(queue != NULL);
  assert(picture != NULL);

  if (queue->size == 0)
  {
    *picture = NULL;
    return;
  }

  *picture = &queue->pictures[queue->rd_index];
}

static bool picture_queue_consume(PictureQueue* queue)
{
  assert(queue != NULL);

  if (++queue->rd_index == PICTURE_QUEUE_SIZE)
    queue->rd_index = 0;

  if (SDL_LockMutex(queue->mutex) != 0)
  {
    fprintf(stderr, "SDL_LockMutex() error: %s\n", SDL_GetError());
    return false;
  }

  queue->size -= 1;

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

static void picture_queue_destroy(PictureQueue* queue)
{
  assert(queue != NULL);

  if (queue->mutex == NULL || queue->cond == NULL)
    return;

  if (SDL_LockMutex(queue->mutex) != 0)
  {
    fprintf(stderr, "SDL_LockMutex() error: %s\n", SDL_GetError());
  }

  queue->wr_index = 0;
  queue->rd_index = 0;
  queue->size = 0;

  int i = 0;
  Picture* picture = &queue->pictures[0];
  for (; i < PICTURE_QUEUE_SIZE; i++, picture++)
  {
    /* TO DO: lock picture mutex before destroying the texture */
    if (picture->texture)
      SDL_DestroyTexture(picture->texture);
    picture->texture = NULL;
    picture->width = 0;
    picture->height = 0;
    if (picture->mutex)
      SDL_DestroyMutex(picture->mutex);
    picture->mutex = NULL;
  }

  if (SDL_UnlockMutex(queue->mutex) != 0)
  {
    fprintf(stderr, "SDL_UnlockMutex() error: %s\n", SDL_GetError());
  }
  
  SDL_DestroyMutex(queue->mutex);
  queue->mutex = NULL;
  SDL_DestroyCond(queue->cond);
  queue->cond = NULL;    
}

static double get_audio_clock(void)
{
  double pts = audio_clock;
  int audio_chunk_length = audio_data_length - audio_buffer_index;

  AVCodecContext* codec_ctx = audio_decoder.codec_ctx;
  assert(codec_ctx);

  int bytes_per_sec = CHANNELS_NUMBER * BYTES_PER_SAMPLE * codec_ctx->sample_rate;

  pts -= (double)audio_chunk_length / bytes_per_sec;
  
  return pts;
}

static double get_video_clock(void)
{
  double delta = (av_gettime() - video_current_pts_time) / 1000000.0;
  return video_current_pts + delta;
}

static double get_external_clock(void)
{
  return av_gettime() / 1000000.0;
}

static double get_master_clock(void)
{
  if (sync_master_source == SyncVideoMaster)
  {
    return get_video_clock();
  }
  else if (sync_master_source == SyncAudioMaster)
  {
    return get_audio_clock();
  }
  else
  {
    return get_external_clock();
  }

  assert(false);
  return 0.0;
}

static void picture_display(const Picture* picture)
{
  assert(picture != NULL);
  assert(picture->mutex != NULL);

  SDL_Texture* texture = picture->texture;
  SDL_Renderer* renderer = video_device.renderer;

  assert(texture != NULL);
  assert(renderer != NULL);


  if (SDL_LockMutex(picture->mutex) != 0)
  {
    fprintf(stderr, "SDL_LockMutex() error: %s\n", SDL_GetError());
    return ;
  }

  if (SDL_RenderClear(renderer) != 0)
  {
    fprintf(stderr, "SDL_RenderClear() error: %s\n", SDL_GetError());
    if (SDL_UnlockMutex(picture->mutex) != 0)
      fprintf(stderr, "SDL_UnlockMutex() error: %s\n", SDL_GetError());
    return ;
  }

  
  int window_width = 0, window_height = 0;
  SDL_GetWindowSize(video_device.window, &window_width, &window_height);

  int h = window_height;
  int w = ((int)rint(h * aspect_ratio))& -3;
  if (w > window_width)
  {
    w = window_width;
    h = ((int)rint(w / aspect_ratio)) & -3;
  }

  SDL_Rect rect;

  rect.x = (window_width - w) / 2;
  rect.y = (window_height - h) / 2;
  rect.w = w;
  rect.h = h;

  if (SDL_RenderCopy(renderer, texture, NULL, &rect) != 0)
  {
    fprintf(stderr, "SDL_RenderCopy() error: %s\n", SDL_GetError());
    if (SDL_UnlockMutex(picture->mutex) != 0)
      fprintf(stderr, "SDL_UnlockMutex() error: %s\n", SDL_GetError());
    return ;    
  }

  if (SDL_UnlockMutex(picture->mutex) != 0)
  {
    fprintf(stderr, "SDL_UnlockMutex() error: %s\n", SDL_GetError());    
    return ;
  }

  SDL_RenderPresent(renderer);  
}


static void audio_callback(void* userdata, Uint8* stream, int length)
{
  int decoded_length = 0, audio_chunk_length = 0;

  SDL_memset(stream, 0, length);

  while (length > 0)
  {    
    if (audio_buffer_index >= audio_data_length)
    {
      /* Get more data. */
      decoded_length = decode_audio(userdata, audio_buffer, sizeof(audio_buffer));
      if (decoded_length < 0)
      {
        /* Error when decoding, output silence. */
        audio_data_length = 1024;
        memset(audio_buffer, 0, audio_data_length);
      }
      else
      {
        audio_data_length = synchronize_audio(audio_buffer, decoded_length);
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

static void refresh_screen(void)
{
  Picture* picture = NULL;

  picture_queue_peek(&picture_queue, &picture);
  if (!picture)
    schedule_screen_refresh(1);
  else
  {
    double delay = picture->pts - frame_last_pts;
    if (delay <= 0.0 || delay >= 1.0)
      delay = frame_last_delay;

    frame_last_delay = delay;
    frame_last_pts = picture->pts;

    video_current_pts = picture->pts;
    video_current_pts_time = av_gettime();

    if (sync_master_source != SyncVideoMaster)
    {
      double ref_clock = get_master_clock();
      double diff = picture->pts - ref_clock;

      double sync_threshold = (delay > SYNC_THRESHOLD) ? delay : SYNC_THRESHOLD;
      if (fabs(diff) < NOSYNC_THRESHOLD)
      {
        if (diff <= -sync_threshold)
          delay = 0.0;
        else if (diff >= sync_threshold)
          delay *= 2.0;
      }      
    }

    frame_timer += delay;
    double actual_delay = frame_timer - (av_gettime() / 1000000.0);
    if (actual_delay < 0.010)
      actual_delay = 0.010;

    schedule_screen_refresh((int)(actual_delay * 1000.0 + 0.5));
    picture_display(picture);

    picture_queue_consume(&picture_queue);
  }
}

static Uint32 on_refresh_screen_timer(Uint32 interval, void* param)
{	
  SDL_Event event;
  event.type = REFRESH_SCREEN_EVENT;

  if (SDL_PushEvent(&event) < 0)
    fprintf(stderr, "SDL_PushEvent() failed: %s\n", SDL_GetError());

  return 0;
}

static void schedule_screen_refresh(Uint32 delay_ms)
{
  SDL_AddTimer(delay_ms, on_refresh_screen_timer, NULL);  
}

static void synchronize_video(const AVFrame* frame, double pts)
{
  if (pts != 0.0)
    video_clock = pts;

  double frame_delay = av_q2d(media_container.video_stream->time_base);
  frame_delay += frame->repeat_pict * (frame_delay * 0.5);  /* documentation says: extra_delay = repeat_pict / (2*fps)  */
  video_clock += frame_delay;
}

static int decode_audio(void* userdata, uint8_t* audio_buffer, int buffer_size)
{
  AVCodecContext* codec_ctx = audio_decoder.codec_ctx;
  assert(codec_ctx != NULL);

  struct SwrContext* swr_ctx = audio_resample.swr_ctx;
  assert(swr_ctx != NULL);

  AVFrame* frame = audio_resample.frame;
  assert(frame != NULL);

  assert(audio_buffer != NULL);

  AVPacket packet;
  int ret = packet_queue_get(&audio_queue, &packet, true);

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

  if (packet.size == 5 && strncmp((const char*)packet.data, "FLUSH", 5) == 0)
  {
    av_packet_unref(&packet);
    avcodec_flush_buffers(codec_ctx);
    return 0;
  }  

  ret = avcodec_send_packet(codec_ctx, &packet);
  if (ret < 0)
  {
    fprintf(stderr, "Error sending packet (%s)\n", av_err2str(ret));
    av_packet_unref(&packet);
    return -1;
  }

  int length = 0;

  while (ret >= 0)
  {
    ret = avcodec_receive_frame(codec_ctx, frame);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
      break;
    else if (ret < 0)
    {
      fprintf(stderr, "Error while receiving frame from the decoder (%s)\n", av_err2str(ret));
      break;
    }


    int nsamples_delay = swr_get_delay(swr_ctx, codec_ctx->sample_rate);
    int nsamples = av_rescale_rnd(nsamples_delay + frame->nb_samples, codec_ctx->sample_rate, SAMPLE_RATE, AV_ROUND_UP);

    uint8_t** samples = NULL;
    if (av_samples_alloc_array_and_samples(&samples, NULL, CHANNELS_NUMBER, nsamples, AV_SAMPLE_FMT_S16, 0) < 0)
    {
      fprintf(stderr, "Could not allocate memory to store audiosamples!\n");
      break;
    }

    if (av_samples_alloc(samples, NULL, CHANNELS_NUMBER, nsamples, AV_SAMPLE_FMT_S16, 0) < 0)
    {
      fprintf(stderr, "Could not allocate memory to store audiosamples!\n");
      av_freep(&samples);
      break;
    }

    int nsamples_converted = swr_convert(swr_ctx, samples, nsamples, 
                                        (const uint8_t **)frame->data, frame->nb_samples);

    /* int nbytes = nsamples_converted * CHANNELS_NUMBER * BYTES_PER_SAMPLE; */
    int nbytes = av_samples_get_buffer_size(NULL, CHANNELS_NUMBER, nsamples_converted, AV_SAMPLE_FMT_S16, 1);

    assert((length + nbytes) <= buffer_size);
    memcpy(audio_buffer, samples[0], nbytes);

    audio_buffer += nbytes;
    length += nbytes;

    if (samples)
      av_freep(&samples[0]);

    av_freep(&samples);

    /* double pts = audio_clock; */
    audio_clock += (double)nbytes / (double)(CHANNELS_NUMBER * BYTES_PER_SAMPLE * codec_ctx->sample_rate);
  }

  av_packet_unref(&packet);

  return length;
}

static int synchronize_audio(uint8_t* audio_buffer, int samples_size)
{
  if (sync_master_source != SyncAudioMaster)
  {
    double ref_clock = get_master_clock();
    double diff = get_audio_clock() - ref_clock;

    if (diff < NOSYNC_THRESHOLD)
    {
      /* accumulate the diffs */
      audio_diff_cum = diff + audio_diff_avg_coeff * audio_diff_cum;

      if (audio_diff_avg_count < AUDIO_DIFF_AVG_NB)
      {
        audio_diff_avg_count++;
      }
      else
      {
        double avg_diff = audio_diff_cum * (1.0 - audio_diff_avg_coeff);
        /* shrink/expand audio buffer */
        if (fabs(avg_diff) >= AUDIO_DIFF_THRESHOLD)
        {
          const int n = CHANNELS_NUMBER * BYTES_PER_SAMPLE;
          int size = samples_size + ((int)(diff * audio_decoder.codec_ctx->sample_rate) * n);
          int min_size = size * ((100 - SAMPLE_CORRECTION_PERCENT_MAX) / 100);
          int max_size = size * ((100 + SAMPLE_CORRECTION_PERCENT_MAX) / 100);

          if (size < min_size)
          {
            size = min_size;
          }
          else if (size > max_size)
          {
            size = max_size;
          }

          if (size < samples_size)
          {
            /* remove samples */
            samples_size = size;
          }
          else
          {
            /* add samples by copying final samples */
            int nb = size - samples_size;
            uint8_t* samples_end = audio_buffer + samples_size - n;
            uint8_t* dst = samples_end + n;
            while (nb > 0)
            {
              memcpy(dst, samples_end, n);
              dst += n;
              nb -= n;
            }
            samples_size = size;
          }
        }
      }
    }
    else
    {
      /* the difference is too large, reset diff stuff */
      audio_diff_avg_count = 0;
      audio_diff_cum = 0.0;
    }
  }

  return samples_size;
}


static int parse_container(void* userdata)
{
  AVPacket packet;
  SDL_Event event;
  
  /* initialize packet, set data to NULL, let the demuxer fill it */
  av_init_packet(&packet);
  packet.data = NULL;
  packet.size = 0;

  int ret = 0;
  const size_t ERROR_BUFFER_SIZE = 512;
  char error_buffer[ERROR_BUFFER_SIZE];
   
  /* read frames from the file */
  while (!finished)
  {
    if (seek_req)
    {
      if (media_container.audio_stream_idx >= 0)
      {
        int audio_stream_idx = media_container.audio_stream_idx;
        AVStream* audio_stream = media_container.format_ctx->streams[audio_stream_idx];
        int64_t seek_pos_audio = av_rescale_q(seek_pos, AV_TIME_BASE_Q, audio_stream->time_base);
        if ((ret = av_seek_frame(media_container.format_ctx, audio_stream_idx, seek_pos_audio, seek_flags)) < 0)
        {
          fprintf(stderr, "Could not seek frame in audio stream!\n");
          fprintf(stderr, "\tdetails: %s\n", av_make_error_string(error_buffer, ERROR_BUFFER_SIZE, ret));
        }
        else
        {
          AVPacket flush_packet;
          av_init_packet(&flush_packet);
          flush_packet.size = 5;
          flush_packet.data = "FLUSH";
          packet_queue_flush(&audio_queue);
          if (!packet_queue_put(&audio_queue, &flush_packet))
          {
            fprintf(stderr, "Could not enqueue flush packet (audio)!\n");
          }
        }
      }

      if (media_container.video_stream_idx >= 0)
      {
        int video_stream_idx = media_container.video_stream_idx;
        AVStream* video_stream = media_container.format_ctx->streams[video_stream_idx];
        int64_t seek_pos_video = av_rescale_q(seek_pos, AV_TIME_BASE_Q, video_stream->time_base);

        if ((ret = av_seek_frame(media_container.format_ctx, video_stream_idx, seek_pos_video, seek_flags)) < 0)
        {
          fprintf(stderr, "Could not seek frame in video stream!\n");
          fprintf(stderr, "\tdetails: %s\n", av_make_error_string(error_buffer, ERROR_BUFFER_SIZE, ret));
        }
        else
        {
          AVPacket flush_packet;
          av_init_packet(&flush_packet);
          flush_packet.size = 5;
          flush_packet.data = "FLUSH";
          packet_queue_flush(&video_queue);
          if (!packet_queue_put(&video_queue, &flush_packet))
          {
            fprintf(stderr, "Could not enqueue flush packet (video)!\n");
          }
        }
      }      

      seek_req = 0;
    }

    if (audio_queue.num_of_bytes > MAX_AUDIOQ_SIZE || video_queue.num_of_bytes > MAX_VIDEOQ_SIZE)
    {
      SDL_Delay(10);
    }

    if (av_read_frame(media_container.format_ctx, &packet) < 0)
    {
      if (media_container.format_ctx->pb->error == 0)
      {
        /* not an error, wait for user input */
        SDL_Delay(100);
      }
      else
      {
        fprintf(stderr, "Could not read frame from the media container!\n");
        av_packet_unref(&packet);
        goto parse_fail;
      }
    }

    if (packet.stream_index == media_container.video_stream_idx)
    {
      if (!packet_queue_put(&video_queue, &packet))
      {
        fprintf(stderr, "Could not enqueue video packet!\n");
        av_packet_unref(&packet);
        goto parse_fail;
      }
    }
    else if (packet.stream_index == media_container.audio_stream_idx)
    {
      if (!packet_queue_put(&audio_queue, &packet))
      {
        fprintf(stderr, "Could not enqueue audio packet!\n");
        av_packet_unref(&packet);
        goto parse_fail;
      }

      if (packet.pts != AV_NOPTS_VALUE)
        audio_clock = av_q2d(media_container.audio_stream->time_base) * packet.pts;
    }
    else
    {
      av_packet_unref(&packet);
    }
  }

  goto parse_end;

parse_fail:
  event.type = QUIT_EVENT;
  if (SDL_PushEvent(&event) < 0)
    fprintf(stderr, "SDL_PushEvent() failed: %s\n", SDL_GetError());

parse_end:
  return 0;
}

static int decode_video(void* userdata)
{
  AVCodecContext* codec_ctx = video_decoder.codec_ctx;
  assert(codec_ctx != NULL);

  AVFrame* frame = av_frame_alloc();
  if (!frame)
  {
    fprintf(stderr, "Could not allocate video frame!\n");
    return -1;
  }


  AVPacket packet;
  SDL_Event event;

  for (;;)
  {
    if (finished)
      break;

    int ret = packet_queue_get(&video_queue, &packet, true);
    if (ret == -1)
    {
      fprintf(stderr, "Could not get a video packet from the queue - an error !\n");
      goto decode_video_fail;
    }
    else if (ret == 0)
    {
      fprintf(stderr, "Could not get a video packet from the queue - no data !\n");
      goto decode_video_fail;
    }

    if (packet.size == 5 && strncmp((const char*)packet.data, "FLUSH", 5) == 0)
    {
      av_packet_unref(&packet);
      avcodec_flush_buffers(codec_ctx);
      continue;
    }

    ret = avcodec_send_packet(codec_ctx, &packet);
    if (ret < 0)
    {
      fprintf(stderr, "Error sending packet (%s)\n", av_err2str(ret));
      av_packet_unref(&packet);
      goto decode_video_fail;
    }


    while (ret >= 0)
    {
      ret = avcodec_receive_frame(codec_ctx, frame);
      if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        break;
      else if (ret < 0)
      {
        fprintf(stderr, "Error while receiving frame from the decoder (%s)\n", av_err2str(ret));
        goto decode_video_fail;
      }

      double pts = frame->best_effort_timestamp;
      pts *= av_q2d(media_container.video_stream->time_base);

      if (pts == 0)
      {
      	pts = video_clock;
        synchronize_video(frame, 0.0);
      }
      else
      {
        synchronize_video(frame, pts);
      }

      if (!picture_queue_put_frame(&picture_queue, frame, pts))
      {
        if (finished)
          goto decode_video_end;

        fprintf(stderr, "picture_queue_put_frame() failed!\n");
        goto decode_video_fail;
      }

    }

    av_packet_unref(&packet);
  }

  goto decode_video_end;

decode_video_fail:
  event.type = QUIT_EVENT;
  if (SDL_PushEvent(&event) < 0)
    fprintf(stderr, "SDL_PushEvent() failed: %s\n", SDL_GetError());

decode_video_end:

  if (frame != NULL)
    av_frame_free(&frame);

  return 0;
}
