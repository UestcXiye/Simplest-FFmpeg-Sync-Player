// Simplest FFmpeg Sync Player.cpp : �������̨Ӧ�ó������ڵ㡣
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

// ����
// LNK2019 �޷��������ⲿ���� __imp__fprintf���÷����ں��� _ShowError �б�����
// LNK2019 �޷��������ⲿ���� __imp____iob_func���÷����ں��� _ShowError �б�����

// ����취��
// ������ı������汾���ڵ�ǰ����汾����Ҫ��������Դ����vs2017���±��룬����û�а������Դ�룬��·��ͨ��
// Ȼ��鵽˵��stdin, stderr, stdout �⼸������vs2015����ǰ�Ķ���ò�һ�������Ա���
// ��������أ�����ʹ��{ *stdin,*stdout,*stderr }�����Լ�����__iob_func()
#pragma comment(lib,"legacy_stdio_definitions.lib")
extern "C"
{
	FILE __iob_func[3] = { *stdin, *stdout, *stderr };
}

char av_error[AV_ERROR_MAX_STRING_SIZE] = { 0 };
#define av_err2str(errnum) av_make_error_string(av_error, AV_ERROR_MAX_STRING_SIZE, errnum)

#define MAX_VIDEO_PIC_NUM  1 // ��󻺴����ͼƬ��

#define AV_SYNC_THRESHOLD 0.01 // ͬ����С��ֵ
#define AV_NOSYNC_THRESHOLD 10.0 //  ��ͬ����ֵ

// Packet ����
typedef struct PacketQueue
{
	AVPacketList* first_pkt, *last_pkt; // ͷ��βָ��
	int nb_packets; // packet ������
	SDL_mutex* mutex; // SDL ������
} PacketQueue;

// ����Ƶͬ��ʱ��ģʽ
enum {
	AV_SYNC_AUDIO_MASTER, // ������ƵΪ��ʱ�ӣ�����Ƶͬ������Ƶ�ϣ�Ĭ��ѡ��
	AV_SYNC_VIDEO_MASTER, // ������ƵΪ��ʱ�ӣ�����Ƶͬ������Ƶ�ϣ����Ƽ�
	AV_SYNC_EXTERNAL_CLOCK, // ѡ��һ���ⲿʱ��Ϊ��׼�����Ƽ�
};

// Buffer:
// |-----------|-------------|
// chunk-------pos---len-----|
static Uint8* audio_chunk;
static Uint32 audio_len;
static Uint8* audio_pos;

SDL_Window* sdlWindow = nullptr; // ����
SDL_Renderer* sdlRenderer = nullptr; // ��Ⱦ��
SDL_Texture* sdlTexture = nullptr; // ����
SDL_Rect sdlRect; // ��Ⱦ��ʾ���

AVFormatContext* pFormatCtx = NULL;
AVPacket* pkt;
AVFrame* video_frame, *audio_frame;
int ret;
int video_index = -1, audio_index = -1;

// �����ļ�·��
char in_filename[] = "������ʲô��.mov";
// char in_filename[] = "��Щ�꣬����һ��׷��Ů��.mp4";
// char in_filename[] = "cuc_ieschool.mp4";

int frame_width = 1280;
int frame_height = 720;

// ��Ƶ����
AVCodec* video_pCodec = nullptr;
AVCodecContext* video_pCodecCtx = nullptr;

typedef struct video_pic
{
	AVFrame frame;

	float clock; // ��ʾʱ��
	float duration; // ����ʱ��
	int frame_NUM; // ֡��
} video_pic;

video_pic v_pic[MAX_VIDEO_PIC_NUM]; // ��Ƶ������ౣ����֡����
int pic_count = 0; // �Ѵ洢ͼƬ����

// ��Ƶ����
AVCodec* audio_pCodec = nullptr;
AVCodecContext* audio_pCodecCtx = nullptr;

PacketQueue video_pkt_queue; // ��Ƶ֡����
PacketQueue audio_pkt_queue; // ��Ƶ֡����

// ͬ��ʱ�ӣ�������ƵΪ��ʱ��
int av_sync_type = AV_SYNC_AUDIO_MASTER;

int64_t audio_callback_time;

double video_clock; // ��Ƶʱ��
double audio_clock; // ��Ƶʱ��

// SDL ��Ƶ�����ṹ
SDL_AudioSpec audio_spec;

// ��ʼ�� SDL ��������ص�����Ƶ����
int initSDL();
// �ر� SDL ���ͷ���Դ
void closeSDL();
// SDL ��Ƶ�ص�����
void fill_audio_pcm2(void* udata, Uint8* stream, int len);

// fltp תΪ packed ��ʽ
void fltp_convert_to_f32le(float* f32le, float* fltp_l, float* fltp_r, int nb_samples, int channels)
{
	for (int i = 0; i < nb_samples; i++)
	{
		f32le[i * channels] = fltp_l[i];
		f32le[i * channels + 1] = fltp_r[i];
	}
}

// ��һ�� AVPacket ������Ӧ�Ķ�����
void put_AVPacket_into_queue(PacketQueue *q, AVPacket* packet)
{
	SDL_LockMutex(q->mutex); // ����
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

	SDL_UnlockMutex(q->mutex); // ����
}

// �� AVPacket ������ȡ����һ��֡
static void packet_queue_get(PacketQueue* q, AVPacket *pkt2)
{
	while (true)
	{
		AVPacketList* pkt1 = nullptr;
		// һֱȡ��ֱ�������������ݣ��ͷ���
		pkt1 = q->first_pkt;
		if (pkt1)
		{
			SDL_LockMutex(q->mutex); // ����
			q->first_pkt = pkt1->next;

			if (!q->first_pkt)
				q->last_pkt = nullptr;

			q->nb_packets--;
			SDL_UnlockMutex(q->mutex); // ����
			// pkt2 ָ������ȡ��֡
			*pkt2 = pkt1->pkt;
			// �ͷ�֡
			av_free(pkt1);
			break;
		}
		else
		{
			// ��������ʱû��֡���ȴ�
			SDL_Delay(1);
		}
	}
	return;
}

// ��Ƶ���벥���߳�
int video_play_thread(void * data)
{
	AVPacket video_pkt = { 0 };
	// ȡ����
	while (true)
	{
		// ����Ƶ֡������ȡ��һ�� AVPacket
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
			// printf("֡����%3d\n", video_pCodecCtx->frame_number);
			fflush(stdout); // �����������������ѻ������������

							// video_clock = video_pCodecCtx->frame_number * duration
			video_clock = av_q2d(video_pCodecCtx->time_base) * video_pCodecCtx->ticks_per_frame * 1000 * video_pCodecCtx->frame_number;
			// printf("��Ƶʱ�ӣ�%f ms\n", video_clock);
			double duration = av_q2d(video_pCodecCtx->time_base) * video_pCodecCtx->ticks_per_frame * 1000;

			// �������������
			SDL_UpdateYUVTexture(sdlTexture, nullptr, // �������� rect��Ϊ nullptr ��ʾȫ������
				video_frame->data[0], video_frame->linesize[0],
				video_frame->data[1], video_frame->linesize[1],
				video_frame->data[2], video_frame->linesize[2]);

			sdlRect.x = 0;
			sdlRect.y = 0;
			sdlRect.w = frame_width;
			sdlRect.h = frame_height;

			// ������Ⱦ��������
			SDL_RenderClear(sdlRenderer);
			// ����������������Ⱦƽ����
			SDL_RenderCopy(sdlRenderer, sdlTexture, NULL, &sdlRect);
			// ��ת��������ǰ̨��ʾ
			SDL_RenderPresent(sdlRenderer);

			// ����������һ֡���ӳ�ʱ�䣬��ʵ��ͬ��
			double delay = duration;
			double diff = video_clock - audio_clock; // ʱ���
			if (fabs(diff) <= duration) // ʱ�����һ֡��Χ�ڱ�ʾ��������ʱ����ʱ��
				delay = duration;
			else if (diff > duration) // ��Ƶʱ�ӱ���Ƶʱ�ӿ죬�Ҵ���һ֡��ʱ�䣬��ʱ 2 ��
				delay *= 2;
			else if (diff < -duration) // ��Ƶʱ�ӱ���Ƶʱ�������ҳ���һ֡ʱ�䣬�������ŵ�ǰ֡
				delay = 0;

			printf("frame: %d, delay: %lf ms\n", video_pCodecCtx->frame_number, delay);

			SDL_Delay(delay);
		}
	}
	return 0;
}

// ��Ƶ���벥���߳�
int audio_play_thread(void* data)
{
	AVPacket audio_pkt = { 0 };
	// ȡ����
	while (true)
	{
		// ����Ƶ֡������ȡ��һ�� AVPacket
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
			* �����ǵõ��������������ݽ��д��������������ݵ���������Ӧ�Ĵ���
			* �� AAC ������� PCM ��H.264 ������� YUV���ȵȡ�
			*/

			// ���ݲ�����ʽ����ȡÿ��������ռ���ֽ���
			int data_size = av_get_bytes_per_sample(audio_pCodecCtx->sample_fmt);
			if (data_size < 0)
			{
				// This should not occur, checking just for paranoia
				fprintf(stderr, "Failed to calculate data size.\n");
				break;
			}

			// nb_samples: AVFrame ����Ƶ֡������channels: ͨ����
			int pcm_buffer_size = data_size * audio_frame->nb_samples * audio_pCodecCtx->channels;
			uint8_t* pcm_buffer = (uint8_t*)malloc(pcm_buffer_size);
			memset(pcm_buffer, 0, pcm_buffer_size);
			// ת��Ϊ packed ģʽ
			fltp_convert_to_f32le((float*)pcm_buffer, (float*)audio_frame->data[0], (float*)audio_frame->data[1],
				audio_frame->nb_samples, audio_pCodecCtx->channels);
			// ʹ�� SDL ����
			// Set audio buffer (PCM data)
			audio_chunk = pcm_buffer;
			audio_len = pcm_buffer_size;
			audio_pos = audio_chunk;

			audio_clock = audio_frame->pts * av_q2d(audio_pCodecCtx->time_base) * 1000;
			// printf("��Ƶʱ��: %f ms\n", audio_clock);
			// Wait until finish
			while (audio_len > 0)
			{
				// ʹ�� SDL_Delay ���� 1ms ���ӳ٣��õ�ǰ������ʣ��δ���ŵĳ��ȴ��� 0 ���ǰ����ӳٽ��еȴ�
				SDL_Delay(1);
			}

			free(pcm_buffer);
		}
	}
	return 0;
}

// �⸴���߳�
int open_file_thread(void* data)
{
	// ��ȡһ�� AVPacket
	while (av_read_frame(pFormatCtx, pkt) >= 0)
	{
		if (pkt->stream_index == video_index)
		{
			// ������Ƶ����
			put_AVPacket_into_queue(&video_pkt_queue, pkt);
		}
		else if (pkt->stream_index == audio_index)
		{
			// ������Ƶ����
			put_AVPacket_into_queue(&audio_pkt_queue, pkt);
		}
		else
		{
			// �����Ǵ����ݶ�����ȡ������ʹ�������Ҫ�ͷſռ䣨AVPacket��
			// ���򱻵����ڴ�й©�����³���ռ���ڴ�Խ��Խ��
			av_packet_unref(pkt);
		}
	}
	return 0;
}

int main(int argc, char * argv[])
{
	// ��ý���ļ�
	ret = avformat_open_input(&pFormatCtx, in_filename, 0, 0);
	if (ret < 0)
	{
		printf("Couldn't open input file.\n");
		return -1;
	}
	// ��ȡý���ļ���Ϣ���� pFormatCtx ��ֵ
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
	av_dump_format(pFormatCtx, 0, in_filename, 0); // ��ӡ�����ļ���Ϣ
	printf("-------------------------------------------------\n");

	// ������Ƶ����Ϣ�� codec_id �ҵ���Ӧ�Ľ�����
	video_pCodec = avcodec_find_decoder(pFormatCtx->streams[video_index]->codecpar->codec_id);
	if (!video_pCodec)
	{
		printf("Video codec not found.\n");
		return -1;
	}
	// ������Ƶ������������
	video_pCodecCtx = avcodec_alloc_context3(video_pCodec);
	// ������Ƶ����Ϣ����Ƶ��������������
	avcodec_parameters_to_context(video_pCodecCtx, pFormatCtx->streams[video_index]->codecpar);
	// �õ���Ƶ�Ŀ�Ⱥ͸߶�
	frame_width = pFormatCtx->streams[video_index]->codecpar->width;
	frame_height = pFormatCtx->streams[video_index]->codecpar->height;
	// ����Ƶ�������͹���������������
	if (avcodec_open2(video_pCodecCtx, video_pCodec, nullptr))
	{
		printf("Could not open video codec.\n");
		return -1;
	}

	// ������Ƶ����Ϣ�� codec_id �ҵ���Ӧ�Ľ�����
	audio_pCodec = avcodec_find_decoder(pFormatCtx->streams[audio_index]->codecpar->codec_id);
	if (!audio_pCodec)
	{
		printf("Audio codec not found.\n");
		return -1;
	}
	// ������Ƶ������������
	audio_pCodecCtx = avcodec_alloc_context3(audio_pCodec);
	// ������Ƶ����Ϣ����Ƶ��������������
	avcodec_parameters_to_context(audio_pCodecCtx, pFormatCtx->streams[audio_index]->codecpar);
	// ����Ƶ�������͹���������������
	if (avcodec_open2(audio_pCodecCtx, audio_pCodec, nullptr))
	{
		printf("Could not open audio codec.\n");
		return -1;
	}

	// ����һ�� AVPacket �ṹ
	pkt = av_packet_alloc();

	// ����һ�� AVFrame �ṹ������Ž���������
	video_frame = av_frame_alloc();
	audio_frame = av_frame_alloc();

	// ��ʼ�� SDL
	initSDL();

	// ����������
	video_pkt_queue.mutex = SDL_CreateMutex();
	audio_pkt_queue.mutex = SDL_CreateMutex();

	// ���� SDL ��Ƶ���Ų���
	audio_spec.freq = audio_pCodecCtx->sample_rate; // ������
	audio_spec.format = AUDIO_F32LSB; // ��Ƶ���ݲ�����ʽ
	audio_spec.channels = audio_pCodecCtx->channels; // ͨ����
	audio_spec.silence = 0; // ��Ƶ���徲��ֵ
	audio_spec.samples = audio_pCodecCtx->frame_size; // ÿһ֡�Ĳ����������������� 512��1024�����ò����ʿ��ܻᵼ�¿���
	audio_spec.callback = fill_audio_pcm2; // ��Ƶ���Żص�

	// ��ϵͳ��Ƶ�豸
	if (SDL_OpenAudio(&audio_spec, NULL) < 0)
	{
		printf("Can't open audio.\n");
		return -1;
	}
	// ��ʼ����
	SDL_PauseAudio(0);
	// ���� SDL �߳�
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

	// ���ٻ�����
	SDL_DestroyMutex(video_pkt_queue.mutex);
	SDL_DestroyMutex(audio_pkt_queue.mutex);

	// �ر� SDL
	closeSDL();

	// �ͷ� FFmpeg �����Դ
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

// SDL ��ʼ��
int initSDL()
{
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER))
	{
		printf("Could not initialize SDL - %s\n", SDL_GetError());
		return -1;
	}

	// �������� SDL_Window
	sdlWindow = SDL_CreateWindow("Simplest FFmpeg Sync Player", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		frame_width, frame_height, SDL_WINDOW_SHOWN);
	if (sdlWindow == nullptr)
	{
		printf("SDL: Could not create window - exiting:%s\n", SDL_GetError());
		return -1;
	}

	// ������Ⱦ�� SDL_Renderer
	sdlRenderer = SDL_CreateRenderer(sdlWindow, -1, 0);
	if (sdlRenderer == nullptr)
	{
		printf("SDL: Could not create renderer - exiting:%s\n", SDL_GetError());
		return -1;
	}

	// �������� SDL_Texture
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

/* SDL ��Ƶ�ص�����
*
* ��ʼ���ź󣬻�����Ƶ�������߳������ûص�������������Ƶ���ݵĲ��䣬��������ÿ�β��� 4096 ���ֽ�
* The audio function callback takes the following parameters:
* stream: A pointer to the audio buffer to be filled
* len: The length (in bytes) of the audio buffer
*
*/
void fill_audio_pcm2(void* udata, Uint8* stream, int len)
{
	// ��ȡ��ǰϵͳʱ��
	audio_callback_time = av_gettime();

	// SDL 2.0
	SDL_memset(stream, 0, len);

	if (audio_len == 0) /* Only play if we have data left */
		return;
	/* Mix as much data as possible */
	len = ((Uint32)len > audio_len ? audio_len : len);
	/* �������ź���
	* dst: Ŀ�����ݣ�����ǻص���������� stream ָ��ָ��ģ�ֱ��ʹ�ûص��� stream ָ�뼴��
	* src: ��Ƶ���ݣ�����ǽ���Ҫ���ŵ���Ƶ���ݻ쵽 stream ����ȥ����ô�������������Ҫ���Ĳ��ŵ�����
	* len: ��Ƶ���ݵĳ���
	* volume: ��������Χ 0~128 ��SAL_MIX_MAXVOLUME Ϊ 128�����õ���������������Ӳ��������
	*/
	SDL_MixAudio(stream, audio_pos, len, SDL_MIX_MAXVOLUME / 2);
	audio_pos += len;
	audio_len -= len;
}

// �ر� SDL
void closeSDL()
{
	// �ر���Ƶ�豸
	SDL_CloseAudio();
	// �ͷ� SDL ��Դ
	SDL_DestroyWindow(sdlWindow);
	sdlWindow = nullptr;
	SDL_DestroyRenderer(sdlRenderer);
	sdlRenderer = nullptr;
	SDL_DestroyTexture(sdlTexture);
	sdlTexture = nullptr;
	// �˳� SDL ϵͳ
	SDL_Quit();
}
