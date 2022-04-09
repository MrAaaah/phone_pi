gcc main.c pa_ringbuffer.c -lwiringPi -lportaudio -lrt -lm -lasound -ljack -lpthread -D_REENTRANT && sudo ./a.out
