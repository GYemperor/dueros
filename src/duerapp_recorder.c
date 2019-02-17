/**
 * Copyright (2017) Baidu Inc. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/**
 * File: duerapp_recorder.c
 * Auth: Renhe Zhang (v_zhangrenhe@baidu.com)
 * Desc: Record module function implementation.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#ifdef ENABLE_TCP_NODELAY
#include <netinet/tcp.h>
#endif
#include <pthread.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <semaphore.h>

#include "duerapp_recorder.h"
#include "duerapp_config.h"
#include "lightduer_voice.h"
#include "lightduer_dcs_router.h"

#include "snowboy-detect-c-wrapper.h"

#define ALSA_PCM_NEW_HW_PARAMS_API
#define SAMPLE_RATE (16000)
#define FRAMES_INIT (16)
#define FRAMES_SIZE (2) // bytes / sample * channels
#define CHANNEL (1)

static int s_duer_rec_snd_fd=-1;
static int s_duer_rec_recv_fd=-1;
struct sockaddr_in s_duer_rec_addr;

static duer_rec_state_t s_duer_rec_state = RECORDER_STOP;
static pthread_t s_rec_threadID;
static sem_t s_rec_sem;
static duer_rec_config_t *s_index = NULL;
static bool s_is_suspend = false;
static bool s_is_baidu_rec_start = false;
static pthread_t s_rec_send_threadID;

extern 	void event_record_start();

static void recorder_thread()
{
	fd_set fdwrite;
	int value=0;

	const char resource_filename[] = "resources/common.res";
	//const char model_filename[] = "snowboy/resources/models/snowboy.umdl";
	const char model_filename[] = "resources/models/keywords.pmdl,resources/models/peppapig.pmdl";
	const char sensitivity_str[] = "0.5,0.5";
	float audio_gain = 1.1;
	bool apply_frontend = false;
	
	// Initializes Snowboy detector.
	SnowboyDetect* detector = SnowboyDetectConstructor(resource_filename,
	                                                 model_filename);
	SnowboyDetectSetSensitivity(detector, sensitivity_str);
	SnowboyDetectSetAudioGain(detector, audio_gain);
	SnowboyDetectApplyFrontend(detector, apply_frontend);

	// Initializes PortAudio. You may use other tools to capture the audio.
	value = SnowboyDetectSampleRate(detector);
	DUER_LOGI("samplerate:%d\n",value);
	
	value = SnowboyDetectNumChannels(detector);
	DUER_LOGI("channels:%d\n",value);
	
	value = SnowboyDetectBitsPerSample(detector);
	DUER_LOGI("bits:%d\n",value);
	
    pthread_detach(pthread_self());
    snd_pcm_hw_params_get_period_size(s_index->params, &(s_index->frames), &(s_index->dir));

    if (s_index->frames < 0) {
        DUER_LOGE("Get period size failed!");
        return;
    }
    s_index->size = s_index->frames * FRAMES_SIZE;
    int16_t *buffer = NULL;
    if (buffer) {
        free(buffer);
        buffer = NULL;
    }
	
    buffer = (int16_t *)malloc(s_index->size);
    if (!buffer) {
        DUER_LOGE("malloc buffer failed!\n");
    } else {
        memset(buffer, 0, s_index->size);
    }
	
    while (1)
    {
        int ret = snd_pcm_readi(s_index->handle, buffer, s_index->frames);

        if (ret == -EPIPE) {
            DUER_LOGE("an overrun occurred!");
            snd_pcm_prepare(s_index->handle);
        } else if (ret < 0) {
            DUER_LOGE("read: %s", snd_strerror(ret));
        } else if (ret != (int)s_index->frames) {
            DUER_LOGE("read %d frames!", ret);
        } else {
            // do nothing
        }
		
	    int result = SnowboyDetectRunDetection(detector,
                                             buffer, s_index->size>>1, false);
        if (result > 0) {
            DUER_LOGI("Hotword %d detected!\n", result);
			duer_dcs_dialog_cancel();
			event_record_start();
        }
		
		if((RECORDER_START == s_duer_rec_state)&&s_is_baidu_rec_start){
			
			struct timeval time = {0, 0};
			
			FD_ZERO(&fdwrite);
    		FD_SET(s_duer_rec_snd_fd, &fdwrite);
			
    		if(select(s_duer_rec_snd_fd + 1, NULL, &fdwrite, NULL, &time)>0){
				send(s_duer_rec_snd_fd,buffer,s_index->size,0);
			}else{
				DUER_LOGI("overloap!!!!!!!!!\n");
			}
		}
    }
	
    if (buffer) {
        free(buffer);
        buffer = NULL;
    }
	
    snd_pcm_drain(s_index->handle);
    snd_pcm_close(s_index->handle);
	
    if(s_index) {
        free(s_index);
        s_index = NULL;
    }	
	SnowboyDetectDestructor(detector);	  
	return;
}

static void recorder_data_send_thread()
{
    char *buffer = NULL;
	int size = s_index->size;
	fd_set fdread;
	struct timeval time = {0, 0};
	int recvlen=0;
	
    pthread_detach(pthread_self());

	DUER_LOGI("recorder_data_send_thread start!\n");	
	s_is_baidu_rec_start = false;
	
	while(1)
	{
		FD_ZERO(&fdread);
		FD_SET(s_duer_rec_recv_fd, &fdread);
		if(select(s_duer_rec_recv_fd + 1, &fdread, NULL, NULL, &time)>0){
			recv(s_duer_rec_recv_fd, buffer, size, 0);
		}else{
			break;
		}
	}
	
	DUER_LOGI("flush data end %d!\n",s_duer_rec_state);
	s_is_baidu_rec_start = true;
    duer_voice_start(16000);
	
    buffer = (char *)malloc(size);
    if (!buffer) {
        DUER_LOGE("malloc buffer failed!\n");
    } else {
        memset(buffer, 0, size);
    }
	
    while (RECORDER_START == s_duer_rec_state)
    {
		FD_ZERO(&fdread);
		time.tv_sec = 1;
		time.tv_usec = 0;
		
		FD_SET(s_duer_rec_recv_fd, &fdread);
		if(select(s_duer_rec_recv_fd + 1, &fdread, NULL, NULL, &time)>0){
			recvlen = recv(s_duer_rec_recv_fd, buffer, size, 0);
			if(recvlen>0){
				printf(".&.");
				duer_voice_send(buffer, s_index->size);
			}else{
				DUER_LOGE("recv fail!\n");
			}
		}
    }
	
	s_is_baidu_rec_start = false;
    duer_voice_stop();
	
    if(s_is_suspend) {
        duer_voice_terminate();
        s_is_suspend = false;
    }
    if (buffer) {
        free(buffer);
        buffer = NULL;
    }
	
    if(sem_post(&s_rec_sem)) {
        DUER_LOGE("sem_post failed.");
    }

	return ;
	
}

static int duer_open_alsa_pcm()
{
    int ret = DUER_OK;
    int result = (snd_pcm_open(&(s_index->handle), "default", SND_PCM_STREAM_CAPTURE, 0));
    if (result < 0)
    {
        DUER_LOGE("unable to open pcm device: %s", snd_strerror(ret));
        ret = DUER_ERR_FAILED;
    }
    return ret;
}

static int duer_set_pcm_params()
{
    int ret = DUER_OK;
    snd_pcm_hw_params_alloca(&(s_index->params));
    snd_pcm_hw_params_any(s_index->handle, s_index->params);
    snd_pcm_hw_params_set_access(s_index->handle, s_index->params,
                                 SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(s_index->handle, s_index->params,
                                 SND_PCM_FORMAT_S16_LE);
    snd_pcm_hw_params_set_channels(s_index->handle, s_index->params,
                                   CHANNEL);
    snd_pcm_hw_params_set_rate_near(s_index->handle, s_index->params,
                                    &(s_index->val), &(s_index->dir));
    snd_pcm_hw_params_set_period_size_near(s_index->handle, s_index->params,
                                           &(s_index->frames), &(s_index->dir));

    int result = snd_pcm_hw_params(s_index->handle, s_index->params);
    if (result < 0)    {
        DUER_LOGE("unable to set hw parameters: %s", snd_strerror(ret));
        ret = DUER_ERR_FAILED;
    }
    return ret;
}

int duer_recorder_start()
{
	int ret = 0;

	DUER_LOGI("duer_recorder_start %d!",s_duer_rec_state);
	
    do {
		
		if(sem_wait(&s_rec_sem)) {
			DUER_LOGE("sem_wait failed.");
			break;
		}
		
        if (RECORDER_STOP == s_duer_rec_state) {
			s_duer_rec_state = RECORDER_START;
            ret = pthread_create(&s_rec_send_threadID, NULL, (void *)recorder_data_send_thread, NULL);
            if(ret != 0)
            {
            	s_duer_rec_state = RECORDER_STOP;
                DUER_LOGE("Create recorder pthread error!");
                break;
            } else {
                pthread_setname_np(s_rec_send_threadID, "recorder");
            }
            
        }
        else{
            DUER_LOGI("Recorder Start failed! state:%d", s_duer_rec_state);
        }
		
        return DUER_OK;
		
    } while(0);
	
    return DUER_ERR_FAILED;
}

int duer_recorder_stop()
{
    int ret = DUER_OK;
	
	DUER_LOGI("duer_recorder_stop! state:%d", s_duer_rec_state);
	
    if (RECORDER_START == s_duer_rec_state) {
        s_duer_rec_state = RECORDER_STOP;
		s_is_baidu_rec_start = false;
    } else {
        ret = DUER_ERR_FAILED;
        DUER_LOGI("Recorder Stop failed! state:%d", s_duer_rec_state);
    }
	
    return ret;
}

int duer_recorder_suspend()
{
    int ret = duer_recorder_stop();
    if (DUER_OK == ret) {
        s_is_suspend = true;
    } else {
        DUER_LOGI("Recorder Stop failed! state:%d", s_duer_rec_state);
    }
    return ret;
}

duer_rec_state_t duer_get_recorder_state()
{
    return s_duer_rec_state;
}

int duer_hotwords_detect_start(void)
{
	int ret=0;
	
    s_duer_rec_addr.sin_family = AF_INET;
    s_duer_rec_addr.sin_port = htons(19290);
	s_duer_rec_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
	
    if(sem_init(&s_rec_sem, 0, 1)) {
        DUER_LOGE("Init s_rec_sem failed.");
        return -1;
    }
	
    s_index = (duer_rec_config_t *)malloc(sizeof(duer_rec_config_t));
    if (!s_index) {
		DUER_LOGE("malloc fail");
        return -1;
    }
	
    memset(s_index, 0, sizeof(duer_rec_config_t));
    s_index->frames = FRAMES_INIT;
    s_index->val = SAMPLE_RATE; // pcm sample rate
    
    do{
		s_duer_rec_snd_fd =  socket(AF_INET, SOCK_DGRAM,IPPROTO_UDP);
		if(s_duer_rec_snd_fd <0){
			DUER_LOGE("open send socket failed");
			break;
		}
		
		ret = connect(s_duer_rec_snd_fd, (struct sockaddr *)&s_duer_rec_addr, sizeof(s_duer_rec_addr));
		if(ret!=0){
			DUER_LOGE("socket connect failed");
			break;
		}
		
		s_duer_rec_recv_fd =  socket(AF_INET, SOCK_DGRAM,IPPROTO_UDP);
		if(s_duer_rec_recv_fd <0){
			DUER_LOGE("open recv socket failed");
			break;
		}

		ret = bind(s_duer_rec_recv_fd, (struct sockaddr *) &s_duer_rec_addr, sizeof(s_duer_rec_addr));
		if(ret<0){
			DUER_LOGE("socket bind failed");
			break;
		}
		
	    ret = duer_open_alsa_pcm();
	    if (ret != DUER_OK) {
	        DUER_LOGE("open pcm failed");
	        break;
	    }
		
	    ret = duer_set_pcm_params();
	    if (ret != DUER_OK) {
	        DUER_LOGE("open pcm failed");
	        break;
	    }

	    ret = pthread_create(&s_rec_threadID, NULL, (void *)recorder_thread, NULL);
	    if(ret != 0){
	        DUER_LOGE("Create recorder pthread error!");
	        break;
	    } else {
	        pthread_setname_np(s_rec_threadID, "recorder");
	    }
    }while(0);

	if(ret!=0){
		if(s_index) {
        	free(s_index);
        	s_index = NULL;
    	}
		if(s_duer_rec_recv_fd>=0){
			close(s_duer_rec_recv_fd);
			s_duer_rec_recv_fd = -1;
		}
		
		if(s_duer_rec_snd_fd>=0){
			close(s_duer_rec_snd_fd);
			s_duer_rec_snd_fd = -1;
		}		
	}
	
    return ret;
	
}
