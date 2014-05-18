/*
 * Jack output driver. This file is part of Shairport.
 * Copyright (c) Gleb Pomykalov 2014
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */


#include <stdio.h>
#include <unistd.h>
#include <memory.h>
#include "common.h"
#include "audio.h"
#include <jack/jack.h>
#include <jack/ringbuffer.h>


unsigned long counter=0;
jack_port_t *left, *right;
jack_client_t *client;
jack_ringbuffer_t *r_buffer;

// #include <pulse/simple.h>
// #include <pulse/error.h>

// static pa_simple *pa_dev = NULL;
// static int pa_error;

static void help(void) {
    printf("    -c client_name      set the jack client name\n"
           "    -s server_name      set the jack server name\n"
          );
}

#define max(x,y) (((x)>(y)) ? (x) : (y))
#define min(x,y) (((x)<(y)) ? (x) : (y))


#define SAMPLE_MAX_16BIT  32768.0f

void sample_move_dS_s16_volume(jack_default_audio_sample_t* dst, char *src, jack_nframes_t nsamples, unsigned long src_skip, float volume) 
 {
    /* ALERT: signed sign-extension portability !!! */
    while (nsamples--) {
        *dst = (*((short *) src)) / SAMPLE_MAX_16BIT * volume;
        dst++;
        src += src_skip;
    }
 }  

int jack_callback (jack_nframes_t nframes, void *arg)
{
    jack_default_audio_sample_t *out1, *out2;

    out1 = (jack_default_audio_sample_t*)jack_port_get_buffer (left, nframes);
    out2 = (jack_default_audio_sample_t*)jack_port_get_buffer (right, nframes);

    jack_nframes_t nframes_left = nframes;
    int wrotebytes = 0;

    if (jack_ringbuffer_read_space(r_buffer) == 0) {
        // just write silence
        memset(out1, 0, nframes * sizeof(jack_default_audio_sample_t));
        memset(out2, 0, nframes * sizeof(jack_default_audio_sample_t)); 
    } else {

        jack_ringbuffer_data_t rb_data[2];

        jack_ringbuffer_get_read_vector(r_buffer, rb_data);

        while (nframes_left > 0 && rb_data[0].len >= 4) {

            jack_nframes_t towrite_frames = (rb_data[0].len) / (sizeof(short) * 2);
            towrite_frames = min(towrite_frames, nframes_left);

            sample_move_dS_s16_volume(
                out1 + (nframes - nframes_left), 
                (char *) rb_data[0].buf, 
                towrite_frames, 
                sizeof(short) * 2, 
                1.0f
            );
            sample_move_dS_s16_volume(
                out2 + (nframes - nframes_left), 
                (char *) rb_data[0].buf + sizeof(short), 
                towrite_frames, 
                sizeof(short) * 2, 
                1.0f);

            wrotebytes = towrite_frames * sizeof(short) * 2;
            nframes_left -= towrite_frames;

            jack_ringbuffer_read_advance(r_buffer, wrotebytes);
            jack_ringbuffer_get_read_vector(r_buffer, rb_data);

        }

        if (nframes_left > 0) {
                // write silence
            memset(out1 + (nframes - nframes_left), 0, (nframes_left) * sizeof(jack_default_audio_sample_t));
            memset(out2 + (nframes - nframes_left), 0, (nframes_left) * sizeof(jack_default_audio_sample_t));       
        }
    }

    return 0;    
}

static int init(int argc, char **argv) {
    const char **ports;
    const char *client_name="shairport"; //FIXME
    const char *server_name = NULL;
    jack_options_t options = JackNullOption;
    jack_status_t status;
    const char *data = NULL; //FIXME: need to pass data to callback?

    r_buffer = jack_ringbuffer_create(sizeof(jack_default_audio_sample_t) * 44100 * 2*10);

//    memset(r_buffer->buf, 0, r_buffer->size);

//TODO: parse client name and server name

    /* open a client connection to the JACK server */

    client = jack_client_open (client_name, options, &status, server_name);
    if (client == NULL) {
        fprintf (stderr, "jack_client_open() failed, "
             "status = 0x%2.0x\n", status);
        if (status & JackServerFailed) {
            fprintf (stderr, "Unable to connect to JACK server\n");
        }
        exit (1);
    }
    if (status & JackServerStarted) {
        printf("JACK server started\n");
    }
    if (status & JackNameNotUnique) {
        client_name = jack_get_client_name(client);
        printf("unique name `%s' assigned\n", client_name);
    }

    jack_set_process_callback (client, jack_callback, r_buffer);

//FIXME: handle shutdowns:
//    jack_on_shutdown (client, jack_shutdown, 0);
//

    left = jack_port_register (client, "left",
                      JACK_DEFAULT_AUDIO_TYPE,
                      JackPortIsOutput, 0);

    right = jack_port_register (client, "right",
                      JACK_DEFAULT_AUDIO_TYPE,
                      JackPortIsOutput, 0);

    if ((left == NULL) || (right == NULL)) {
        die("no more JACK ports available\n");
    }

    /* Tell the JACK server that we are ready to roll.  Our
     * process() callback will start running now. */

    if (jack_activate (client)) {
        die("cannot activate client");
    }

    ports = jack_get_ports (client, NULL, NULL,
                JackPortIsPhysical|JackPortIsInput);
    if (ports == NULL) {
        die("no physical playback ports\n");
    }

    if (jack_connect (client, jack_port_name (left), ports[0])) {
        die("cannot connect output ports\n");
    }

    if (jack_connect (client, jack_port_name (right), ports[1])) {
        die("cannot connect output ports\n");
    }

    jack_free (ports);
//    
//    /* install a signal handler to properly quits jack client */
//#ifdef WIN32
//    signal(SIGINT, signal_handler);
//    signal(SIGABRT, signal_handler);
//    signal(SIGTERM, signal_handler);
//#else
//    signal(SIGQUIT, signal_handler);
//    signal(SIGTERM, signal_handler);
//    signal(SIGHUP, signal_handler);
//    signal(SIGINT, signal_handler);
//#endif

    return 0;
}

static void deinit(void) {
    if (client)
         jack_client_close (client);
    client = NULL;  
}

static void start(int sample_rate) {
    if (sample_rate != 44100)
        die("unexpected sample rate!");
}

static void play(short buf[], int samples) {
    usleep(7300);
    jack_ringbuffer_write (r_buffer, (char *)buf, samples * sizeof(jack_default_audio_sample_t));
}

static void stop(void) {
    // if (pa_simple_drain(pa_dev, &pa_error) < 0)
    //     fprintf(stderr, __FILE__": pa_simple_drain() failed: %s\n", pa_strerror(pa_error));
}

audio_output audio_jack = {
    .name = "jack",
    .help = &help,
    .init = &init,
    .deinit = &deinit,
    .start = &start,
    .stop = &stop,
    .play = &play,
    .volume = NULL
};
