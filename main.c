#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <wiringPi.h>

#include <portaudio.h>
#include "pa_ringbuffer.h"

#define SAMPLE_RATE (44100)
#define FRAMES_PER_BUFFER (512)
#define NUM_SECONDS (10)
#define NUM_CHANNELS (1)
#define NUM_WRITES_PER_BUFFER (4)
#define SAMPLE_TYPE (paInt16)

int switchPin = 22;
int wifiSwitchPin = 26;
int ledWifi = 2;
int ledRec = 0;

typedef short SAMPLE;

typedef struct
{
	char riff[4];
	int fileSize; //overall size
	char wave[4];

	// chunk
	char fmt[4];
	int lengthFormatData;
	short typeFormat;
	short numChannels;
	int sampleRate;
	int sampleBitsChannels;
	short bitSampleChannels;
	short bitsPerSample;

	char data[4];
	int dataSize; // data size (overall - 44 bytes)
}
wavHeader_t;

typedef struct
{
	unsigned frameIndex;
	int threadSyncFlag;
	SAMPLE * ringBufferData;
	PaUtilRingBuffer ringBuffer;
	FILE * file;
	pthread_t threadHandle;
} 
paTestData;


void * threadFunctionWriteToRawFile(void * arg)
{
	paTestData * pData = (paTestData*)arg;

	pData->threadSyncFlag = 0;

	while (1)
	{
		ring_buffer_size_t elementsInBuffer = PaUtil_GetRingBufferReadAvailable(&pData->ringBuffer);
		if ((elementsInBuffer >= pData->ringBuffer.bufferSize / NUM_WRITES_PER_BUFFER) || pData->threadSyncFlag)
		{
			void * ptr[2] = { 0 };
			ring_buffer_size_t sizes[2] = { 0 };

			ring_buffer_size_t elementsRead = PaUtil_GetRingBufferReadRegions(&pData->ringBuffer, elementsInBuffer, ptr + 0, sizes + 0, ptr + 1, sizes + 1);

			if (elementsRead > 0)
			{
				int i;
				for (i = 0 ; i < 2 && ptr[i] != NULL ; ++i)
				{
					fwrite(ptr[i], pData->ringBuffer.elementSizeBytes, sizes[i], pData->file);
				}
				PaUtil_AdvanceRingBufferReadIndex(&pData->ringBuffer, elementsRead);
			}

			if (pData->threadSyncFlag)
			{
				break;
			}
		}

		Pa_Sleep(20);
	}

	pData->threadSyncFlag = 0;

	return 0;
}

int startThread(paTestData * pData)
{
	if (pthread_create(&pData->threadHandle, NULL, threadFunctionWriteToRawFile, pData) < 0)
	{
		printf("pthread_create error for thread\n");
		return 1;
	}


	pData->threadSyncFlag = 1;

	while (pData->threadSyncFlag)
	{
		Pa_Sleep(10);
	}

	return paNoError;
}

void stopThread(paTestData * pData)
{
	pData->threadSyncFlag = 1;
	while (pData->threadSyncFlag)
	{
		Pa_Sleep(10);
	}
	void * ret;
	pthread_join(pData->threadHandle, &ret);
}

//static paTestData data;

int totalSampleCount = 0;


static ring_buffer_size_t rbs_min(ring_buffer_size_t a, ring_buffer_size_t b)
{
	return (a < b) ? a : b;
}

int paStreamCallback(const void * input, void * output,
		unsigned long frameCount, // nb data in the buffer
		const PaStreamCallbackTimeInfo * timeInfo,
		PaStreamCallbackFlags statusFlags,
		void * userData) 
{
	paTestData * pData = (paTestData *)userData;
	ring_buffer_size_t elementsWriteable = PaUtil_GetRingBufferWriteAvailable(&pData->ringBuffer);
	ring_buffer_size_t elementsToWrite = rbs_min(elementsWriteable, (ring_buffer_size_t)(frameCount * NUM_CHANNELS));
	const SAMPLE * rptr = (const SAMPLE *)input;

	pData->frameIndex += PaUtil_WriteRingBuffer(&pData->ringBuffer, rptr, elementsToWrite);

	return 0;
}

unsigned nextPowerOf2(unsigned val)
{
	val--;
	val = (val >> 1) | val;
	val = (val >> 2) | val;
	val = (val >> 4) | val;
	val = (val >> 8) | val;
	val = (val >> 16) | val;
	return ++val;
}

int setupI2CStream(paTestData * userData, PaStream ** stream)
{
	// init port audio lib
	int err = Pa_Initialize();
	if (err != paNoError)
	{
		printf("Error while initializing PortAudio: %s\n",
				Pa_GetErrorText(err));
		return 1;
	}

	// look for the i2s device index, by iterating through the device list
	int i2sDeviceIndex = -1;
	const PaDeviceInfo * deviceInfo = NULL;

	PaDeviceIndex deviceCount = Pa_GetDeviceCount();

	for (int i = 0 ; i < deviceCount ; i++)
	{
		deviceInfo = Pa_GetDeviceInfo(i);
		if (strstr(deviceInfo->name, "i2s"))
		{
			i2sDeviceIndex = i;
			break;
		}
	}

	if (i2sDeviceIndex == -1)
	{

		printf("Couldn't find i2s device\n");
		return 1;
	}

	printf("\ni2s device found at index %d: %s\n", i2sDeviceIndex, deviceInfo->name);
	printf("- Max input channels: %d\n", deviceInfo->maxInputChannels);
	printf("- Max output channels: %f\n", deviceInfo->maxInputChannels);
	printf("- Default low input latency: %f\n", deviceInfo->defaultLowInputLatency);
	printf("- Default low output latency: %f\n", deviceInfo->defaultLowOutputLatency);
	printf("- Default high input latency: %f\n", deviceInfo->defaultHighInputLatency);
	printf("- Default high output latency: %f\n", deviceInfo->defaultHighOutputLatency);
	printf("- Default sample rate: %f\n", deviceInfo->defaultSampleRate);

	*userData = (paTestData){ 0 };
	unsigned numSamples;
	unsigned numBytes;

	numSamples = nextPowerOf2((unsigned)(SAMPLE_RATE * 0.5 * NUM_CHANNELS));
	numBytes = numSamples * sizeof(SAMPLE);
	userData->ringBufferData = (SAMPLE*)malloc(numBytes);
	if (PaUtil_InitializeRingBuffer(&userData->ringBuffer, sizeof(SAMPLE), numSamples, userData->ringBufferData) < 0)
	{
		printf(" fail\n");
		return 1;
	}

	PaStreamParameters inputParams = {
		i2sDeviceIndex, // device id
		NUM_CHANNELS, // channel count
		SAMPLE_TYPE, // sampleFOrmat
		0.02, // suggestedLatency
		NULL // hostAPISpecificStreamInfo
	};

	// open an audio stream
	err = Pa_OpenStream(stream,
			&inputParams,
			NULL,
			SAMPLE_RATE,
			FRAMES_PER_BUFFER, // frames per buffer
			paNoFlag, // flags
			paStreamCallback,
			userData);

	if (err != paNoError)
	{
		printf("Error while trying to open a PortAudio stream: %s\n",
				Pa_GetErrorText(err));
		return 1;
	}

	return 0;
}

int startRecording(const char * filename, FILE ** fh, 
		paTestData * userData, PaStream * stream,
		wavHeader_t * wavHeader)
{
	printf("Record in: %s\n", filename);

	*fh = fopen(filename, "wb");

	*wavHeader = (wavHeader_t){
		{ 'R', 'I', 'F', 'F' },
			0, //overall size
			{ 'W', 'A', 'V', 'E' },
			{ 'f', 'm', 't', ' '},
			16,
			1,
			NUM_CHANNELS,
			SAMPLE_RATE,
			(SAMPLE_RATE * 16 * NUM_CHANNELS) / 8,
			2,
			16,
			{ 'd', 'a', 't', 'a' },
			0 // data size (overall - 44 bytes)
	};

	fwrite(wavHeader, 44, 1, *fh);

	// reset user data for audio recording
	userData->file = *fh;
	PaUtil_FlushRingBuffer(&userData->ringBuffer);

	startThread(userData);

	int err = Pa_StartStream(stream);

	if (err != paNoError)
	{
		printf("Error while trying to start stream: %s\n",
				Pa_GetErrorText(err));
		return 1;
	}
}

int stopRecording(FILE * fh, paTestData * userData, PaStream * stream,
		wavHeader_t * wavHeader)
{
	int err = Pa_StopStream(stream);

	stopThread(userData);


	// update wav header at the file beginning
	wavHeader->dataSize = userData->frameIndex * 2 * NUM_CHANNELS; // data size (overall - 44 bytes)
	wavHeader->fileSize = wavHeader->dataSize + 44; //overall size

	fseek(fh, 0, SEEK_SET);
	fwrite(wavHeader, 44, 1, fh);

	fclose(fh);

	if (err != paNoError)
	{
		printf("Error while trying to end stream: %s\n",
				Pa_GetErrorText(err));
		return 1;
	}

	return 0;
}

int terminatePulseAudio()
{
	int err = Pa_Terminate();
	if (err != paNoError)
	{
		printf("Error while terminating PortAudio: %s\n",
				Pa_GetErrorText(err));
		return 1;
	}

	return 0;
}

int setupGPIO() 
{
	if (wiringPiSetup() < 0)
	{
		printf("Failed to setup wiring pi\n");
		return 1;
	}

	pullUpDnControl(switchPin, PUD_DOWN);
	pinMode(switchPin, INPUT);

	pullUpDnControl(wifiSwitchPin, PUD_DOWN);
	pinMode(wifiSwitchPin, INPUT);

	pinMode(ledWifi, OUTPUT);

	pinMode(ledRec, OUTPUT);


	return 0;
}

void turnWifiOff()
{
	system("ifconfig wlan0 down");
}
	
void turnWifiOn()
{
	system("ifconfig wlan0 up");
}

void getTimeStr(char * str)
{
	time_t t = time(NULL);
	struct tm tm = *localtime(&t);
	sprintf(str, "rec_%d-%02d-%02d_%02d:%02d:%02d.wav", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
}
	
int main(int argc, const char ** argv)
{
	if (setupGPIO() != 0)
		return 1;

	paTestData userData;
	PaStream * stream;

	if (setupI2CStream(&userData, &stream) != 0)
		goto end;

	FILE * fh = NULL;
	wavHeader_t wavHeader;

	int cpt = 0;

	int lastValue = digitalRead(switchPin);
	int wifiSwitchLastValue = -1;

	digitalWrite(ledWifi, 0);
	digitalWrite(ledRec, 0);

	int isRecording = 0;
	int ledRecValue = 0;
	while (1)
	{
		{
			int v = digitalRead(switchPin);

			if (isRecording)
			{
				digitalWrite(ledRec, !((ledRecValue >> 3) & 1));
				ledRecValue++;
			}

			if (v != lastValue)
			{
				if (v)
				{
					printf("Allo?\n");

					digitalWrite(ledRec, 0);
					char filename[255];
					getTimeStr(filename);

					if (startRecording(filename, &fh, &userData, stream, &wavHeader) != 0)
						goto end;
					isRecording = 1;
				}
				else
				{
					if (stopRecording(fh, &userData, stream, &wavHeader) != 0)
						return 1;
					isRecording = 0;
					digitalWrite(ledRec, 0);
					printf("Ciao\n");
				}
			}

			lastValue = v;
		}

		{
			int wifiSwitchValue = digitalRead(wifiSwitchPin);
			if (wifiSwitchValue != wifiSwitchLastValue)
			{
				if (wifiSwitchValue == 1)
				{
					turnWifiOn();
					printf("Turn wifi on\n");
					digitalWrite(ledWifi, 1);
				}
				else
				{
					turnWifiOff();
					printf("Turn wifi off\n");
					digitalWrite(ledWifi, 0);
				}
			}
			wifiSwitchLastValue = wifiSwitchValue;
		}

		int msToWait = 100;
		usleep(msToWait * 1000);
	}

end:
	digitalWrite(ledWifi, 0);
	digitalWrite(ledRec, 0);

	return 0;
}
