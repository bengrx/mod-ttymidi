/*
    This file is part of ttymidi.

    ttymidi is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    ttymidi is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with ttymidi.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <argp.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <jack/jack.h>
#include <jack/intclient.h>
#include <jack/midiport.h>
#include <jack/ringbuffer.h>
#include <linux/serial.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <asm/termios.h>
// MOD specific
#include "mod-semaphore.h"

#define MAX_DEV_STR_LEN               32
#define MAX_MSG_SIZE                1024

/* import ioctl definition here, as we can't include both "sys/ioctl.h" and "asm/termios.h" */
extern int ioctl (int __fd, unsigned long int __request, ...) __THROW;

/* --------------------------------------------------------------------- */
// Globals

volatile bool run;
int serial;

/* --------------------------------------------------------------------- */
// Program options

static struct argp_option options[] =
{
	{"serialdevice" , 's', "DEV" , 0, "Serial device to use. Default = /dev/ttyUSB0", 0 },
	{"baudrate"     , 'b', "BAUD", 0, "Serial port baud rate. Default = 31250", 0 },
	{"verbose"      , 'v', 0     , 0, "For debugging: Produce verbose output", 0 },
	{"printonly"    , 'p', 0     , 0, "Super debugging: Print values read from serial -- and do nothing else", 0 },
	{"quiet"        , 'q', 0     , 0, "Don't produce any output, even when the print command is sent", 0 },
	{"name"		, 'n', "NAME", 0, "Name of the JACK client. Default = ttymidi", 0 },
	{ 0 }
};

typedef struct _arguments
{
	int  silent, verbose, printonly;
	char serialdevice[MAX_DEV_STR_LEN];
	int  baudrate;
	char name[MAX_DEV_STR_LEN];
} arguments_t;

typedef struct _jackdata
{
    jack_client_t* client;
    jack_port_t* port_in;
    jack_port_t* port_out;
    jack_ringbuffer_t* ringbuffer_in;
    jack_ringbuffer_t* ringbuffer_out;
    jack_nframes_t bufsize_compensation;
    sem_t sem;
    bool internal;
    volatile jack_nframes_t last_frame_time;
} jackdata_t;

void exit_cli(int sig)
{
        run = false;
        printf("\rttymidi closing down ... ");

        // unused
        return; (void)sig;
}

static error_t parse_opt (int key, char *arg, struct argp_state *state)
{
	/* Get the input argument from argp_parse, which we
	   know is a pointer to our arguments structure. */
	arguments_t *arguments = state->input;
	int baud_temp;

	switch (key)
	{
		case 'p':
			arguments->printonly = 1;
			break;
		case 'q':
			arguments->silent = 1;
			break;
		case 'v':
			arguments->verbose = 1;
			break;
		case 's':
			if (arg == NULL) break;
			strncpy(arguments->serialdevice, arg, MAX_DEV_STR_LEN);
			break;
		case 'n':
			if (arg == NULL) break;
			strncpy(arguments->name, arg, MAX_DEV_STR_LEN);
			break;
		case 'b':
			if (arg == NULL) break;
			errno = 0;
			baud_temp = strtol(arg, NULL, 0);
			if (errno == EINVAL || errno == ERANGE)
			{
				printf("Baud rate %s is invalid.\n",arg);
				exit(1);
			}
			arguments->baudrate = baud_temp;
			break;
		case ARGP_KEY_ARG:
		case ARGP_KEY_END:
			break;

		default:
			return ARGP_ERR_UNKNOWN;
	}

	return 0;
}

void arg_set_defaults(arguments_t *arguments)
{
	char *serialdevice_temp = "/dev/ttyUSB0";
	arguments->printonly    = 0;
	arguments->silent       = 0;
	arguments->verbose      = 0;
	arguments->baudrate     = 31250;
	char *name_tmp		= (char *)"ttymidi";
	strncpy(arguments->serialdevice, serialdevice_temp, MAX_DEV_STR_LEN);
	strncpy(arguments->name, name_tmp, MAX_DEV_STR_LEN);
}

const char *argp_program_version     = "ttymidi 0.60";
const char *argp_program_bug_address = "tvst@hotmail.com";
static char doc[]       = "ttymidi - Connect serial port devices to JACK MIDI programs!";
static struct argp argp = { options, parse_opt, NULL, doc, NULL, NULL, NULL };
arguments_t arguments;

/* --------------------------------------------------------------------- */
// JACK stuff

static int process_client(jack_nframes_t frames, void* ptr)
{
        if (! run)
            return 0;

        jackdata_t* jackdata = (jackdata_t*) ptr;

        const jack_nframes_t cycle_start = jack_last_frame_time(jackdata->client);

        void* portbuf_in  = jack_port_get_buffer(jackdata->port_in,  frames);
        void* portbuf_out = jack_port_get_buffer(jackdata->port_out, frames);

        // MIDI from serial to JACK
        jack_midi_clear_buffer(portbuf_in);

        char bufc[3 + sizeof(jack_nframes_t)];
        jack_midi_data_t bufj[3];
        size_t bsize;
        jack_nframes_t buf_frame, offset, last_buf_frame = 0;

        while (jack_ringbuffer_read(jackdata->ringbuffer_in, bufc,
                                    3 + sizeof(jack_nframes_t)) == 3 + sizeof(jack_nframes_t))
        {
            memcpy(&buf_frame, bufc+3, sizeof(jack_nframes_t));

            switch (bufc[0] & 0xF0)
            {
            case 0xC0:
            case 0xD0:
                bsize = 2;
                break;
            default:
                bsize = 3;
                break;
            }

            for (size_t i=0; i<bsize; ++i)
                bufj[i] = bufc[i];

            buf_frame += frames - jackdata->bufsize_compensation;

            if (last_buf_frame > buf_frame)
                buf_frame = last_buf_frame;
            else
                last_buf_frame = buf_frame;

            if (buf_frame >= cycle_start)
            {
                offset = buf_frame - cycle_start;

                if (offset >= frames)
                    offset = frames - 1;
            }
            else
            {
                offset = 0;
            }

            jack_midi_event_write(portbuf_in, offset, bufj, bsize);
        }

        // MIDI from JACK to serial
        const uint32_t event_count = jack_midi_get_event_count(portbuf_out);

        if (event_count > 0)
        {
            bool needs_post = false;
            jack_midi_event_t event;

            for (uint32_t i=0; i<event_count; ++i)
            {
                if (jack_midi_event_get(&event, portbuf_out, i) != 0)
                    break;
                if (event.size > 3)
                    continue;

                // set first byte as size
                bufc[0] = event.size;

                // copy the rest
                size_t j=0;
                for (; j<event.size; ++j)
                    bufc[j+1] = event.buffer[j];
                for (; j<3; ++j)
                    bufc[j+1] = 0;

                // ready for ringbuffer
                jack_ringbuffer_write(jackdata->ringbuffer_out, bufc, 4);

                buf_frame = cycle_start + event.time;
                jack_ringbuffer_write(jackdata->ringbuffer_out, (char*)&buf_frame, sizeof(jack_nframes_t));

                needs_post = true;
            }

            if (needs_post)
            {
                // Tell MIDI-out thread we have data
                jackdata->last_frame_time = cycle_start;
                sem_post(&jackdata->sem);
            }
        }

        return 0;
}

bool open_client(jackdata_t* jackdata, jack_client_t* client)
{
        jack_port_t *port_in, *port_out;
        jack_ringbuffer_t *ringbuffer_in, *ringbuffer_out;

        if (client == NULL)
        {
            client = jack_client_open(arguments.name, JackNoStartServer|JackUseExactName, NULL);

            if (client == NULL)
            {
                    fprintf(stderr, "Error opening JACK client.\n");
                    return false;
            }
        }
        else
        {
            jackdata->internal = true;
        }

        if ((port_in = jack_port_register(client, "MIDI_in", JACK_DEFAULT_MIDI_TYPE,
                                          JackPortIsOutput|JackPortIsPhysical|JackPortIsTerminal,
                                          0x0)) == NULL)
        {
                fprintf(stderr, "Error creating input port.\n");
        }

        if ((port_out = jack_port_register(client, "MIDI_out", JACK_DEFAULT_MIDI_TYPE,
                                           JackPortIsInput|JackPortIsPhysical|JackPortIsTerminal,
                                           0x0)) == NULL)
        {
                fprintf(stderr, "Error creating output port.\n");
        }

        if ((ringbuffer_in = jack_ringbuffer_create(MAX_MSG_SIZE*2-1)) == NULL)
        {
                fprintf(stderr, "Error creating JACK input ringbuffer.\n");
        }

        if ((ringbuffer_out = jack_ringbuffer_create(MAX_MSG_SIZE*2-1)) == NULL)
        {
                fprintf(stderr, "Error creating JACK output ringbuffer.\n");
        }

        if (port_in == NULL || port_out == NULL || ringbuffer_in == NULL || ringbuffer_out == NULL)
        {
                jack_client_close(client);
                return false;
        }

        jackdata->client = client;
        jackdata->port_in = port_in;
        jackdata->port_out = port_out;
        jackdata->ringbuffer_in = ringbuffer_in;
        jackdata->ringbuffer_out = ringbuffer_out;
        jackdata->bufsize_compensation = (int)((double)jack_get_buffer_size(jackdata->client) / 10.0 + 0.5);

        jack_set_process_callback(client, process_client, jackdata);

        if (jack_activate(client) != 0)
        {
                fprintf(stderr, "Error activating JACK client.\n");
                jack_client_close(client);
                return false;
        }

        sem_init(&jackdata->sem, 0, 0);

        jack_ringbuffer_mlock(ringbuffer_in);
        jack_ringbuffer_mlock(ringbuffer_out);

        if (jack_port_by_name(client, "mod-host:midi_in") != NULL)
        {
                char ourportname[255];
                sprintf(ourportname, "%s:MIDI_in", jack_get_client_name(client));
                jack_connect(client, ourportname, "mod-host:midi_in");
        }

        return true;
}

void close_client(jackdata_t* jackdata)
{
        jack_deactivate(jackdata->client);
        jack_port_unregister(jackdata->client, jackdata->port_in);
        jack_port_unregister(jackdata->client, jackdata->port_out);
        jack_ringbuffer_free(jackdata->ringbuffer_in);
        jack_ringbuffer_free(jackdata->ringbuffer_out);

        if (! jackdata->internal)
                jack_client_close(jackdata->client);

        sem_destroy(&jackdata->sem);
        bzero(jackdata, sizeof(*jackdata));
}

/* --------------------------------------------------------------------- */
// MIDI stuff

void* write_midi_from_jack(void* ptr)
{
        jackdata_t* jackdata = (jackdata_t*) ptr;

        char bufc[4];
        jack_nframes_t buf_frame, buf_diff, cycle_start;
        size_t size;

        const jack_nframes_t sample_rate = jack_get_sample_rate(jackdata->client);

        /* used for select sleep
         * (compared to usleep which sleeps at *least* x us, select sleeps at *most*)
         */
        fd_set fd;
        FD_ZERO(&fd);
        struct timeval tv = { 0, 0 };

        while (run)
        {
                if (sem_timedwait_secs(&jackdata->sem, 1) != 0)
                        continue;

                if (! run) break;

                cycle_start = jackdata->last_frame_time;
                buf_diff = 0;

                while (jack_ringbuffer_read(jackdata->ringbuffer_out, bufc, 4) == 4)
                {
                        if (jack_ringbuffer_read(jackdata->ringbuffer_out, (char*)&buf_frame,
                                                 sizeof(jack_nframes_t)) != sizeof(jack_nframes_t))
                            continue;

                        if (buf_frame > cycle_start)
                        {
                            buf_diff   = buf_frame - cycle_start - buf_diff;
                            tv.tv_usec = (buf_diff * 1000000) / sample_rate;

                            if (tv.tv_usec > 60 && tv.tv_usec < 10000 /* 10 ms */)
                            {
                                // assume write takes 50 us
                                tv.tv_usec -= 50;
                                select(0, &fd, NULL, NULL, &tv);
                            }
                        }
                        else
                        {
                            buf_diff = 0;
                        }

                        size = (size_t)bufc[0];
                        write(serial, bufc+1, size);
                }
        }

        return NULL;
}

void* read_midi_from_serial_port(void* ptr)
{
        char buf[3 + sizeof(jack_nframes_t)];
        char msg[MAX_MSG_SIZE];
        int msglen;

        jackdata_t* jackdata = (jackdata_t*) ptr;

	/* Lets first fast forward to first status byte... */
	if (!arguments.printonly) {
		do read(serial, buf, 1);
		while (buf[0] >> 7 == 0);
	}

	while (run)
	{
		/*
		 * super-debug mode: only print to screen whatever
		 * comes through the serial port.
		 */

		if (arguments.printonly)
		{
			read(serial, buf, 1);
			printf("%x\t", (int) buf[0]&0xFF);
			fflush(stdout);
			continue;
		}

		/*
		 * so let's align to the beginning of a midi command.
		 */

		int i = 1;

		while (i < 3) {
			read(serial, buf+i, 1);

			if (buf[i] >> 7 != 0) {
				/* Status byte received and will always be first bit!*/
				buf[0] = buf[i];
				i = 1;
			} else {
				/* Data byte received */
				if (i == 2) {
					/* It was 2nd data byte so we have a MIDI event process! */
					i = 3;
				} else {
					switch (buf[0] & 0xF0)
					{
					case 0xC0:
					case 0xD0:
						i = 3;
						break;
					default:
						i = 2;
						break;
					}
				}
			}

		}

		/* print comment message (the ones that start with 0xFF 0x00 0x00 */
		if (buf[0] == (char) 0xFF && buf[1] == (char) 0x00 && buf[2] == (char) 0x00)
		{
			read(serial, buf, 1);
			msglen = buf[0];
			if (msglen > MAX_MSG_SIZE-1) msglen = MAX_MSG_SIZE-1;

			read(serial, msg, msglen);

			if (arguments.silent) continue;

			/* make sure the string ends with a null character */
			msg[msglen] = 0;

			puts("0xFF Non-MIDI message: ");
			puts(msg);
			putchar('\n');
			fflush(stdout);
		}

		/* TODO: sysex support */

		/* parse MIDI message */
		else
		{
			const jack_nframes_t frames = jack_frame_time(jackdata->client);
			memcpy(buf+3, &frames, sizeof(jack_nframes_t));
			jack_ringbuffer_write(jackdata->ringbuffer_in, buf, 3 + sizeof(jack_nframes_t));
		}
	}

	return NULL;
}

/* --------------------------------------------------------------------- */
// Main program

struct termios2 oldtio, newtio;
jackdata_t jackdata;
pthread_t midi_out_thread;

static bool _ttymidi_init(bool exit_on_failure, jack_client_t* client)
{
        /*
         * Open JACK stuff
         */

        if (! open_client(&jackdata, client))
        {
                if (exit_on_failure)
                {
                        fprintf(stderr, "Error creating jack client.\n");
                        exit(-1);
                }
                return false;
        }

        /*
         *  Open modem device for reading and not as controlling tty because we don't
         *  want to get killed if linenoise sends CTRL-C.
         */

        serial = open(arguments.serialdevice, O_RDWR | O_NOCTTY);

        if (serial < 0)
        {
                if (exit_on_failure)
                {
                        perror(arguments.serialdevice);
                        exit(-1);
                }
                return false;
        }

        /* save current serial port settings */
        ioctl(serial, TCGETS2, &oldtio);

        /* clear struct for new port settings */
        bzero(&newtio, sizeof(newtio));

        /*
         * CRTSCTS : output hardware flow control (only used if the cable has
         *            all necessary lines. See sect. 7 of Serial-HOWTO)
         * CS8     : 8n1 (8bit, no parity, 1 stopbit)
         * CLOCAL  : local connection, no modem contol
         * CREAD   : enable receiving characters
         */
        newtio.c_cflag = BOTHER | CS8 | CLOCAL | CREAD; // CRTSCTS removed

        /*
         * IGNPAR : ignore bytes with parity errors
         * ICRNL  : map CR to NL (otherwise a CR input on the other computer will not terminate input)
         *           otherwise make device raw (no other input processing)
         */
        newtio.c_iflag = IGNPAR;

        /* Raw output */
        newtio.c_oflag = 0;

        /*
         * ICANON : enable canonical input
         * disable all echo functionality, and don't send signals to calling program
         */
        newtio.c_lflag = 0; // non-canonical

        /* Speed */
        newtio.c_ispeed = arguments.baudrate;
        newtio.c_ospeed = arguments.baudrate;

        /*
         * set up: we'll be reading 4 bytes at a time.
         */
        newtio.c_cc[VTIME] = 0;     /* inter-character timer unused */
        newtio.c_cc[VMIN]  = 1;     /* blocking read until n character arrives */

        /*
         * now activate the settings for the port
         */
        ioctl(serial, TCSETS2, &newtio);

        // Linux-specific: enable low latency mode (FTDI "nagling off")
        struct serial_struct ser_info;
        ioctl(serial, TIOCGSERIAL, &ser_info);
        ser_info.flags |= ASYNC_LOW_LATENCY;
        ioctl(serial, TIOCSSERIAL, &ser_info);

        if (arguments.printonly)
        {
                printf("Super debug mode: Only printing the signal to screen. Nothing else.\n");
        }

        /*
         * read commands
         */

        run = true;

        /* Give high priority to our threads */
        pthread_attr_t attributes;
        pthread_attr_init(&attributes);
        pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_JOINABLE);
        pthread_attr_setinheritsched(&attributes, PTHREAD_EXPLICIT_SCHED);
        pthread_attr_setscope(&attributes, (client != NULL) ? PTHREAD_SCOPE_PROCESS : PTHREAD_SCOPE_SYSTEM);
        pthread_attr_setschedpolicy(&attributes, SCHED_FIFO);

        struct sched_param rt_param;
        memset(&rt_param, 0, sizeof(rt_param));
        rt_param.sched_priority = 80;

        pthread_attr_setschedparam(&attributes, &rt_param);

        /* Starting thread that is writing jack port data */
        pthread_create(&midi_out_thread, &attributes, write_midi_from_jack, (void*) &jackdata);

        /* And also thread for polling serial data. As serial is currently read in
           blocking mode, by this we can enable ctrl+c quiting and avoid zombie
           alsa ports when killing app with ctrl+z */
        pthread_t midi_in_thread;
        pthread_create(&midi_in_thread, NULL, read_midi_from_serial_port, (void*) &jackdata);

        pthread_attr_destroy(&attributes);
        return true;
}

void _ttymidi_finish(void)
{
        close_client(&jackdata);
        pthread_join(midi_out_thread, NULL);

        /* restore the old port settings */
        ioctl(serial, TCSETS2, &oldtio);
        printf("\ndone!\n");
}

int main(int argc, char** argv)
{
        arg_set_defaults(&arguments);
        argp_parse(&argp, argc, argv, 0, 0, &arguments);

        if (! _ttymidi_init(true, NULL))
                return 1;

        signal(SIGINT, exit_cli);
        signal(SIGTERM, exit_cli);

        while (run)
                usleep(100000); // 100 ms

        _ttymidi_finish();
}

__attribute__ ((visibility("default")))
int jack_initialize(jack_client_t* client, const char* load_init);

int jack_initialize(jack_client_t* client, const char* load_init)
{
        arg_set_defaults(&arguments);

        // Disable logs for internal client
        arguments.silent = 1;
        arguments.verbose = 0;

        if (load_init != NULL && load_init[0] != '\0')
            strncpy(arguments.serialdevice, load_init, MAX_DEV_STR_LEN);

        if (! _ttymidi_init(false, client))
                return 1;

        return 0;
}

__attribute__ ((visibility("default")))
void jack_finish(void);

void jack_finish(void)
{
        run = false;
        _ttymidi_finish();
}
