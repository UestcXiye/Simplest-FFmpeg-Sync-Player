// Simplest FFmpeg Sync Player.cpp : 定义控制台应用程序的入口点。
//

#include "stdafx.h"

#pragma warning(disable:4996)

#include <stdio.h>

#define __STDC_CONSTANT_MACROS

extern "C"
{
#include "libavformat/avformat.h"
#include "libavutil/time.h"
#include "SDL2/SDL.h"
}

// 报错：
// LNK2019 无法解析的外部符号 __imp__fprintf，该符号在函数 _ShowError 中被引用
// LNK2019 无法解析的外部符号 __imp____iob_func，该符号在函数 _ShowError 中被引用

// 解决办法：
// 包含库的编译器版本低于当前编译版本，需要将包含库源码用vs2017重新编译，由于没有包含库的源码，此路不通。
// 然后查到说是stdin, stderr, stdout 这几个函数vs2015和以前的定义得不一样，所以报错。
// 解决方法呢，就是使用{ *stdin,*stdout,*stderr }数组自己定义__iob_func()
#pragma comment(lib,"legacy_stdio_definitions.lib")
extern "C"
{
	FILE __iob_func[3] = { *stdin, *stdout, *stderr };
}

char av_error[AV_ERROR_MAX_STRING_SIZE] = { 0 };
#define av_err2str(errnum) av_make_error_string(av_error, AV_ERROR_MAX_STRING_SIZE, errnum)

#define MAX_VIDEO_PIC_NUM  1 // 最大缓存解码图片数

#define AV_SYNC_THRESHOLD 0.01 // 同步最小阈值
#define AV_NOSYNC_THRESHOLD 10.0 //  不同步阈值

// Packet 队列
typedef struct PacketQueue
{
	AVPacketList* first_pkt, *last_pkt; // 头、尾指针
	int nb_packets; // packet 计数器
	SDL_mutex* mutex; // SDL 互斥量
} PacketQueue;

// 音视频同步时钟模式
enum {
	AV_SYNC_AUDIO_MASTER, // 设置音频为主时钟，将视频同步到音频上，默认选项
	AV_SYNC_VIDEO_MASTER, // 设置视频为主时钟，将音频同步到视频上，不推荐
	AV_SYNC_EXTERNAL_CLOCK, // 选择一个外部时钟为基准，不推荐
};

// Buffer:
// |-----------|-------------|
// chunk-------pos---len-----|
static Uint8* audio_chunk;
static Uint32 audio_len;
static Uint8* audio_pos;

SDL_Window* sdlWindow = nullptr; // 窗口
SDL_Renderer* sdlRenderer = nullptr; // 渲染器
SDL_Texture* sdlTexture = nullptr; // 纹理
SDL_Rect sdlRect; // 渲染显示面积

AVFormatContext* pFormatCtx = NULL;
AVPacket* pkt;
AVFrame* video_frame, *audio_frame;
int ret;
int video_index = -1, audio_index = -1;

// 输入文件路径
char in_filename[] = "春晚是什么？.mov";
// char in_filename[] = "那些年，我们一起追的女孩.mp4";
// char in_filename[] = "cuc_ieschool.mp4";

int frame_width = 1280;
int frame_height = 720;

// 视频解码
AVCodec* video_pCodec = nullptr;
AVCodecContext* video_pCodecCtx = nullptr;

typedef struct video_pic
{
	AVFrame frame;

	float clock; // 显示时钟
	float duration; // 持续时间
	int frame_NUM; // 帧号
} video_pic;

video_pic v_pic[MAX_VIDEO_PIC_NUM]; // 视频解码最多保存四帧数据
int pic_count = 0; // 已存储图片数量

// 音频解码
AVCodec* audio_pCodec = nullptr;
AVCodecContext* audio_pCodecCtx = nullptr;

PacketQueue video_pkt_queue; // 视频帧队列
PacketQueue audio_pkt_queue; // 音频帧队列

// 同步时钟，设置音频为主时钟
int av_sync_type = AV_SYNC_AUDIO_MASTER;

int64_t audio_callback_time;

double video_clock; // 视频时钟
double audio_clock; // 音频时钟

// SDL 音频参数结构
SDL_AudioSpec audio_spec;

// 初始化 SDL 并设置相关的音视频参数
int initSDL();
// 关闭 SDL 并释放资源
void closeSDL();
// SDL 音频回调函数
void fill_audio_pcm2(void* udata, Uint8* stream, int len);

// fltp 转为 packed 形式
void fltp_convert_to_f32le(float* f32le, float* fltp_l, float* fltp_r, int nb_samples, int channels)
{
	for (int i = 0; i < nb_samples; i++)
	{
		f32le[i * channels] = fltp_l[i];
		f32le[i * channels + 1] = fltp_r[i];
	}
}

// 将一个 AVPacket 放入相应的队列中
void put_AVPacket_into_queue(PacketQueue *q, AVPacket* packet)
{
	SDL_LockMutex(q->mutex); // 上锁
	AVPacketList* temp = nullptr;
	temp = (AVPacketList*)av_malloc(sizeof(AVPacketList));
	if (!temp)
	{
		printf("Malloc an AVPacketList error.\n");
		return;
	}

	temp->pkt = *packet;
	temp->next = nullptr;

	if (!q->last_pkt)
		q->first_pkt = temp;
	else
		q->last_pkt->next = temp;

	q->last_pkt = temp;
	q->nb_packets++;

	SDL_UnlockMutex(q->mutex); // 解锁
}

// 从 AVPacket 队列中取出第一个帧
static void packet_queue_get(PacketQueue* q, AVPacket *pkt2)
{
	while (true)
	{
		AVPacketList* pkt1 = nullptr;
		// 一直取，直到队列中有数据，就返回
		pkt1 = q->first_pkt;
		if (pkt1)
		{
			SDL_LockMutex(q->mutex); // 上锁
			q->first_pkt = pkt1->next;

			if (!q->first_pkt)
				q->last_pkt = nullptr;

			q->nb_packets--;
			SDL_UnlockMutex(q->mutex); // 解锁
			// pkt2 指向我们取的帧
			*pkt2 = pkt1->pkt;
			// 释放帧
			av_free(pkt1);
			break;
		}
		else
		{
			// 队列里暂时没有帧，等待
			SDL_Delay(1);
		}
	}
	return;
}

// 视频解码播放线程
int video_play_thread(void * data)
{
	AVPacket video_pkt = { 0 };
	// 取数据
	while (true)
	{
		// 从视频帧队列中取出一个 AVPacket
		packet_queue_get(&video_pkt_queue, &video_pkt);
		// Send packet to decoder
		ret = avcodec_send_packet(video_pCodecCtx, &video_pkt);
		if (ret < 0)
		{
			fprintf(stderr, "Error sending a packet to video decoder.\n", av_err2str(ret));
			return -1;
		}

		while (ret >= 0)
		{
			// Receive frame from decoder
			ret = avcodec_receive_frame(video_pCodecCtx, video_frame);
			if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
				break;
			else if (ret < 0)
			{
				fprintf(stderr, "Error receiving frame from video decoder.\n");
				break;
			}
			// printf("帧数：%3d\n", video_pCodecCtx->frame_number);
			fflush(stdout); // 清空输出缓冲区，并把缓冲区内容输出

							// video_clock = video_pCodecCtx->frame_number * duration
			video_clock = av_q2d(video_pCodecCtx->time_base) * video_pCodecCtx->ticks_per_frame * 1000 * video_pCodecCtx->frame_number;
			// printf("视频时钟：%f ms\n", video_clock);
			double duration = av_q2d(video_pCodecCtx->time_base) * video_pCodecCtx->ticks_per_frame * 1000;

			// 设置纹理的数据
			SDL_UpdateYUVTexture(sdlTexture, nullptr, // 矩形区域 rect，为 nullptr 表示全部区域
				video_frame->data[0], video_frame->linesize[0],
				video_frame->data[1], video_frame->linesize[1],
				video_frame->data[2], video_frame->linesize[2]);

			sdlRect.x = 0;
			sdlRect.y = 0;
			sdlRect.w = frame_width;
			sdlRect.h = frame_height;

			// 清理渲染器缓冲区
			SDL_RenderClear(sdlRenderer);
			// 将纹理拷贝到窗口渲染平面上
			SDL_RenderCopy(sdlRenderer, sdlTexture, NULL, &sdlRect);
			// 翻转缓冲区，前台显示
			SDL_RenderPresent(sdlRenderer);

			// 调整播放下一帧的延迟时间，以实现同步
			double delay = duration;
			double diff = video_clock - audio_clock; // 时间差
			if (fabs(diff) <= duration) // 时间差在一帧范围内表示正常，延时正常时间
				delay = duration;
			else if (diff > duration) // 视频时钟比音频时钟快，且大于一帧的时间，延时 2 倍
				delay *= 2;
			else if (diff < -duration) // 视频时钟比音频时钟慢，且超出一帧时间，立即播放当前帧
				delay = 0;

			printf("frame: %d, delay: %lf ms\n", video_pCodecCtx->frame_number, delay);

			SDL_Delay(delay);
		}
	}
	return 0;
}

// 音频解码播放线程
int audio_play_thread(void* data)
{
	AVPacket audio_pkt = { 0 };
	// 取数据
	while (true)
	{
		// 从音频帧队列中取出一个 AVPacket
		packet_queue_get(&audio_pkt_queue, &audio_pkt);
		// Send packet to decoder
		ret = avcodec_send_packet(audio_pCodecCtx, &audio_pkt);
		if (ret < 0)
		{
			fprintf(stderr, "Error sending a packet to audio decoder.\n", av_err2str(ret));
			return -1;
		}

		while (ret >= 0)
		{
			// Receive frame from decoder
			ret = avcodec_receive_frame(audio_pCodecCtx, audio_frame);
			if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
				break;
			else if (ret < 0)
			{
				fprintf(stderr, "Error receiving frame from audio decoder.\n");
				break;
			}

			/*
			* 下面是得到解码后的裸流数据进行处理，根据裸流数据的特征做相应的处理，
			* 如 AAC 解码后是 PCM ，H.264 解码后是 YUV，等等。
			*/

			// 根据采样格式，获取每个采样所占的字节数
			int data_size = av_get_bytes_per_sample(audio_pCodecCtx->sample_fmt);
			if (data_size < 0)
			{
				// This should not occur, checking just for paranoia
				fprintf(stderr, "Failed to calculate data size.\n");
				break;
			}

			// nb_samples: AVFrame 的音频帧个数，channels: 通道数
			int pcm_buffer_size = data_size * audio_frame->nb_samples * audio_pCodecCtx->channels;
			uint8_t* pcm_buffer = (uint8_t*)malloc(pcm_buffer_size);
			memset(pcm_buffer, 0, pcm_buffer_size);
			// 转换为 packed 模式
			fltp_convert_to_f32le((float*)pcm_buffer, (float*)audio_frame->data[0], (float*)audio_frame->data[1],
				audio_frame->nb_samples, audio_pCodecCtx->channels);
			// 使用 SDL 播放
			// Set audio buffer (PCM data)
			audio_chunk = pcm_buffer;
			audio_len = pcm_buffer_size;
			audio_pos = audio_chunk;

			audio_clock = audio_frame->pts * av_q2d(audio_pCodecCtx->time_base) * 1000;
			// printf("音频时钟: %f ms\n", audio_clock);
			// Wait until finish
			while (audio_len > 0)
			{
				// 使用 SDL_Delay 进行 1ms 的延迟，用当前缓存区剩余未播放的长度大于 0 结合前面的延迟进行等待
				SDL_Delay(1);
			}

			free(pcm_buffer);
		}
	}
	return 0;
}

// 解复用线程
int open_file_thread(void* data)
{
	// 读取一个 AVPacket
	while (av_read_frame(pFormatCtx, pkt) >= 0)
	{
		if (pkt->stream_index == video_index)
		{
			// 加入视频队列
			put_AVPacket_into_queue(&video_pkt_queue, pkt);
		}
		else if (pkt->stream_index == audio_index)
		{
			// 加入音频队列
			put_AVPacket_into_queue(&audio_pkt_queue, pkt);
		}
		else
		{
			// 当我们从数据队列中取出数据使用完后，需要释放空间（AVPacket）
			// 否则被导致内存泄漏，导致程序占用内存越来越大
			av_packet_unref(pkt);
		}
	}
	return 0;
}

int main(int argc, char * argv[])
{
	// 打开媒体文件
	ret = avformat_open_input(&pFormatCtx, in_filename, 0, 0);
	if (ret < 0)
	{
		printf("Couldn't open input file.\n");
		return -1;
	}
	// 读取媒体文件信息，给 pFormatCtx 赋值
	ret = avformat_find_stream_info(pFormatCtx, 0);
	if (ret < 0)
	{
		printf("Couldn't find stream information.\n");
		return -1;
	}

	video_index = -1;
	for (int i = 0; i < pFormatCtx->nb_streams; i++)
	{
		if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
		{
			video_index = i;
			break;
		}
	}
	if (video_index == -1)
	{
		printf("Didn't find a video stream.\n");
		return -1;
	}

	audio_index = -1;
	for (size_t i = 0; i < pFormatCtx->nb_streams; i++)
	{
		if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
		{
			audio_index = i;
			break;
		}
	}
	if (audio_index == -1)
	{
		printf("Didn't find an audio stream.\n");
		return -1;
	}

	// Output Info
	printf("--------------- File Information ----------------\n");
	av_dump_format(pFormatCtx, 0, in_filename, 0); // 打印输入文件信息
	printf("-------------------------------------------------\n");

	// 根据视频流信息的 codec_id 找到对应的解码器
	video_pCodec = avcodec_find_decoder(pFormatCtx->streams[video_index]->codecpar->codec_id);
	if (!video_pCodec)
	{
		printf("Video codec not found.\n");
		return -1;
	}
	// 分配视频解码器上下文
	video_pCodecCtx = avcodec_alloc_context3(video_pCodec);
	// 拷贝视频流信息到视频解码器上下文中
	avcodec_parameters_to_context(video_pCodecCtx, pFormatCtx->streams[video_index]->codecpar);
	// 得到视频的宽度和高度
	frame_width = pFormatCtx->streams[video_index]->codecpar->width;
	frame_height = pFormatCtx->streams[video_index]->codecpar->height;
	// 打开视频解码器和关联解码器上下文
	if (avcodec_open2(video_pCodecCtx, video_pCodec, nullptr))
	{
		printf("Could not open video codec.\n");
		return -1;
	}

	// 根据音频流信息的 codec_id 找到对应的解码器
	audio_pCodec = avcodec_find_decoder(pFormatCtx->streams[audio_index]->codecpar->codec_id);
	if (!audio_pCodec)
	{
		printf("Audio codec not found.\n");
		return -1;
	}
	// 分配音频解码器上下文
	audio_pCodecCtx = avcodec_alloc_context3(audio_pCodec);
	// 拷贝音频流信息到音频解码器上下文中
	avcodec_parameters_to_context(audio_pCodecCtx, pFormatCtx->streams[audio_index]->codecpar);
	// 打开音频解码器和关联解码器上下文
	if (avcodec_open2(audio_pCodecCtx, audio_pCodec, nullptr))
	{
		printf("Could not open audio codec.\n");
		return -1;
	}

	// 申请一个 AVPacket 结构
	pkt = av_packet_alloc();

	// 申请一个 AVFrame 结构用来存放解码后的数据
	video_frame = av_frame_alloc();
	audio_frame = av_frame_alloc();

	// 初始化 SDL
	initSDL();

	// 创建互斥量
	video_pkt_queue.mutex = SDL_CreateMutex();
	audio_pkt_queue.mutex = SDL_CreateMutex();

	// 设置 SDL 音频播放参数
	audio_spec.freq = audio_pCodecCtx->sample_rate; // 采样率
	audio_spec.format = AUDIO_F32LSB; // 音频数据采样格式
	audio_spec.channels = audio_pCodecCtx->channels; // 通道数
	audio_spec.silence = 0; // 音频缓冲静音值
	audio_spec.samples = audio_pCodecCtx->frame_size; // 每一帧的采样点数量，基本是 512、1024，设置不合适可能会导致卡顿
	audio_spec.callback = fill_audio_pcm2; // 音频播放回调

	// 打开系统音频设备
	if (SDL_OpenAudio(&audio_spec, NULL) < 0)
	{
		printf("Can't open audio.\n");
		return -1;
	}
	// 开始播放
	SDL_PauseAudio(0);
	// 创建 SDL 线程
	SDL_CreateThread(open_file_thread, "open_file", nullptr);
	SDL_CreateThread(video_play_thread, "video_play", nullptr);
	SDL_CreateThread(audio_play_thread, "audio_play", nullptr);

	bool quit = false;
	SDL_Event e;
	while (quit == false)
	{
		while (SDL_PollEvent(&e) != 0)
		{
			if (e.type == SDL_QUIT)
			{
				quit = true;
				break;
			}
		}
	}

	// 销毁互斥量
	SDL_DestroyMutex(video_pkt_queue.mutex);
	SDL_DestroyMutex(audio_pkt_queue.mutex);

	// 关闭 SDL
	closeSDL();

	// 释放 FFmpeg 相关资源
	avcodec_close(video_pCodecCtx);
	avcodec_free_context(&video_pCodecCtx);
	avcodec_close(audio_pCodecCtx);
	avcodec_free_context(&audio_pCodecCtx);
	av_packet_free(&pkt);
	av_frame_free(&audio_frame);
	av_frame_free(&video_frame);
	avformat_close_input(&pFormatCtx);

	return 0;
}

// SDL 初始化
int initSDL()
{
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER))
	{
		printf("Could not initialize SDL - %s\n", SDL_GetError());
		return -1;
	}

	// 创建窗口 SDL_Window
	sdlWindow = SDL_CreateWindow("Simplest FFmpeg Sync Player", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		frame_width, frame_height, SDL_WINDOW_SHOWN);
	if (sdlWindow == nullptr)
	{
		printf("SDL: Could not create window - exiting:%s\n", SDL_GetError());
		return -1;
	}

	// 创建渲染器 SDL_Renderer
	sdlRenderer = SDL_CreateRenderer(sdlWindow, -1, 0);
	if (sdlRenderer == nullptr)
	{
		printf("SDL: Could not create renderer - exiting:%s\n", SDL_GetError());
		return -1;
	}

	// 创建纹理 SDL_Texture
	// IYUV: Y + U + V  (3 planes)
	// YV12: Y + V + U  (3 planes)
	sdlTexture = SDL_CreateTexture(sdlRenderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, frame_width, frame_height);
	if (sdlTexture == nullptr)
	{
		printf("SDL: Could not create texture - exiting:%s\n", SDL_GetError());
		return -1;
	}

	sdlRect.x = 0;
	sdlRect.y = 0;
	sdlRect.w = frame_width;
	sdlRect.h = frame_height;

	return 0;
}

/* SDL 音频回调函数
*
* 开始播放后，会有音频其他子线程来调用回调函数，进行音频数据的补充，经过测试每次补充 4096 个字节
* The audio function callback takes the following parameters:
* stream: A pointer to the audio buffer to be filled
* len: The length (in bytes) of the audio buffer
*
*/
void fill_audio_pcm2(void* udata, Uint8* stream, int len)
{
	// 获取当前系统时钟
	audio_callback_time = av_gettime();

	// SDL 2.0
	SDL_memset(stream, 0, len);

	if (audio_len == 0) /* Only play if we have data left */
		return;
	/* Mix as much data as possible */
	len = ((Uint32)len > audio_len ? audio_len : len);
	/* 混音播放函数
	* dst: 目标数据，这个是回调函数里面的 stream 指针指向的，直接使用回调的 stream 指针即可
	* src: 音频数据，这个是将需要播放的音频数据混到 stream 里面去，那么这里就是我们需要填充的播放的数据
	* len: 音频数据的长度
	* volume: 音量，范围 0~128 ，SAL_MIX_MAXVOLUME 为 128，设置的是软音量，不是硬件的音响
	*/
	SDL_MixAudio(stream, audio_pos, len, SDL_MIX_MAXVOLUME / 2);
	audio_pos += len;
	audio_len -= len;
}

// 关闭 SDL
void closeSDL()
{
	// 关闭音频设备
	SDL_CloseAudio();
	// 释放 SDL 资源
	SDL_DestroyWindow(sdlWindow);
	sdlWindow = nullptr;
	SDL_DestroyRenderer(sdlRenderer);
	sdlRenderer = nullptr;
	SDL_DestroyTexture(sdlTexture);
	sdlTexture = nullptr;
	// 退出 SDL 系统
	SDL_Quit();
}
