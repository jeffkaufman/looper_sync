/** @file simple_client.c
 *
 * @brief This simple client demonstrates the most basic features of JACK
 * as they would be used by many applications.
 */

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <jack/jack.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

/* if we have four sound sources (mic, three buffers) then we should
   divide all sounds by 4 before giving them to the speaker.
   Unfortunately, that's too soft on my system, so we only divide by
   2.  There is some danger of clipping. */
#define VOLUME_DECREASE 2

/* hardcoded sample rate so we can make buffer sizes depend on the
   number of samples in 60 seconds */
#define SAMPLE_RATE 48000
#define SECONDS_OF_RECORDING 60

/* how long one buffer is */
#define AMT_MEM SECONDS_OF_RECORDING*SAMPLE_RATE

/* there are three loop buffers represented in loop_bufs.  They are at
   offsets 0, AMT_MEM, and 2*AMT_MEM */
jack_default_audio_sample_t loop_bufs[AMT_MEM*3]; // three buffers

/* which of the three loops is the one that we're synching all the
   other loops to.  If this loop is stopped we'll try to make another
   loop primary.  If no other loop is running, we stop */
int primary;

/* where in the loop buffer we're playing/recording from.  We start at
   0 when we record the first loop. */
int loop_pos = 0;

/* where in the loop buffer to go back around to the beginning again.
   Will be a multiple of nframes 
   
   There's only one loop length at once.  All loops repeat on the same
   cycle.

   Because the loop starts at 0, loop length and loop end are the
   same.
*/
int loop_end = 0;

/* we have three mouse buttons.  On my fake external mouse they're named "all pass", "4", and "3". */
#define MOUSE_A 0
#define MOUSE_4 1
#define MOUSE_3 2
#define MOUSE_None -1

/*** jack stuff ***/
jack_port_t *input_port;
jack_port_t *output_port;
jack_client_t *client;

/*** mouse stuff ***/
#define MAX_MOUSE_READ 1024
int amt_read_mouse = 0;
char mouse_buf[MAX_MOUSE_READ];
int mouse_fd;

/*** state stuff ***/

/* we have two kinds of state: main (int state) and per pedal (int
   pedal_states[3]).  Allowed states are:

   main     pedal_state
   ----     -----------
   OFF      OFF
   PRI_REC  one state (primary) is in REC, all others in OFF
   PLY      one state (primary) is in PLY, all others in any of OFF,
            WAIT_REC, REC, or PLY

*/

/* nothing playing */
#define STATE_OFF 201

/* recording the primary track.  This is the recording that sets the
   loop length */
#define STATE_PRI_REC 202

/* running normally.  Playing at least one track (primary), possibly
   recording or playing others, a loop length is defined */
#define STATE_PLY 203

/* main state */
int state = STATE_OFF;

/* this pedal is off */
#define pSTATE_OFF 301

/* this pedal is not primary, it's been pressed, and we're waiting for
   the beginning of the loop before we start recording. */
#define pSTATE_WAIT_REC 302

/* we're recording to the buffer for this pedal */
#define pSTATE_REC 303

/* we're playing from the buffer for this pedal */
#define pSTATE_PLY 304

/* individual pedal states.  If the main state is OFF then these are
   ignored.  When the main state becomes PRI_REC all but primary is
   set to OFF */
int pedal_states[3];

/* figure out which button is active, if any.  Returns one of MOUSE_A, MOUSE_4, MOUSE_3, or MOUSE_None */
int get_mouse()
{
  if ((amt_read_mouse = read(mouse_fd, mouse_buf, MAX_MOUSE_READ)) == -1){
    if (errno != EINTR && errno != EAGAIN) {
      perror("badness");
      exit(-1);
    }
  }
  else {
    for (int i = 0 ; i < amt_read_mouse ; i++){
      if (mouse_buf[i] == 0x8) {} // mouse up
      else if (mouse_buf[i] == 0x0) {} // padding
      else if (mouse_buf[i] == 0xA) { return MOUSE_A; }
      else if (mouse_buf[i] == 0x9) { return MOUSE_4; }
      else if (mouse_buf[i] == 0xC) { return MOUSE_3; }
      else { printf ("mouse: other (%x)\n", mouse_buf[i]);  }
    }
  }
  return MOUSE_None;
}
        

void respond_to_mouse(int mouse_press) {
  if (mouse_press == MOUSE_None) { return; }

  if (state == STATE_OFF) {
    primary = mouse_press;
    printf ("recording primary %d\n", primary);
    state = STATE_PRI_REC;
    pedal_states[0] = pSTATE_OFF;
    pedal_states[1] = pSTATE_OFF;
    pedal_states[2] = pSTATE_OFF;
    pedal_states[primary] = pSTATE_REC;
    
    loop_pos = 0;
  }
  else if (state == STATE_PRI_REC){
    if (mouse_press == primary) {
      printf ("playing primary %d\n", primary);
      state = STATE_PLY;
      pedal_states[primary] = pSTATE_PLY;
      loop_end = loop_pos;
      loop_pos = 0;
    }
    else { state = STATE_OFF; }
  }
  else if (state == STATE_PLY){
    
    if (mouse_press == primary) {
      for (int pedal = 0 ; pedal < 3 ; pedal++) {
	if (pedal != primary && pedal_states[pedal] == pSTATE_PLY) {
	  primary = pedal;
	  break;
	}
      }
      if (mouse_press == primary) {
	printf ("failed to find new primary\n");
	state = STATE_OFF;
	printf ("off\n");
      }
    }
    else {
      if (pedal_states[mouse_press] == pSTATE_PLY) {
	printf ("stopping %d\n", mouse_press);
	pedal_states[mouse_press] = pSTATE_OFF;
      }
      else {
	printf ("waiting to record secondary %d\n", mouse_press);
	pedal_states[mouse_press] = pSTATE_WAIT_REC;
      }
    }
  }
}

/**
 * The process callback for this JACK application is called in a
 * special realtime thread once for each audio cycle.
 *
 * Moves between states based on mouse actions, then does stuff to
 * input, output, and buffers depending on the current state.
 *
 */
int process (jack_nframes_t nframes, void *arg)
{
	jack_default_audio_sample_t *in, *out;
	in = jack_port_get_buffer (input_port, nframes);
	out = jack_port_get_buffer (output_port, nframes);

	/* move between states apropriately */
	respond_to_mouse(get_mouse());

	
	for (int i = 0 ; i < nframes ; i++) {
	  out[i] = in[i] / VOLUME_DECREASE;
	}
	
	if (state == STATE_OFF) { }
	else
	{
	  for (int pedal = 0 ; pedal < 3 ; pedal++) {

	    if (loop_pos == 0 && pedal != primary) {
	      if (pedal_states[pedal] == pSTATE_WAIT_REC) {
		printf ("recording secondary %d\n", pedal);
		pedal_states[pedal] = pSTATE_REC;
	      }
	      else if (pedal_states[pedal] == pSTATE_REC) {
		printf ("playing secondary %d\n", pedal);
                pedal_states[pedal] = pSTATE_PLY;
              }
	    }

	    if (pedal_states[pedal] == pSTATE_PLY) {
	      for (int i = 0 ; i < nframes ; i++) {
		out[i] += loop_bufs[AMT_MEM*pedal + loop_pos + i] / VOLUME_DECREASE;
	      }
	    }
	    else if (pedal_states[pedal] == pSTATE_REC) {
	      for (int i = 0 ; i < nframes ; i++) {
		loop_bufs[AMT_MEM*pedal + loop_pos + i] = in[i];
	      }
	    }

	  }
	}

	loop_pos += nframes;
	if (state == STATE_PLY && loop_pos >= loop_end) { loop_pos = 0 ;}
	if (loop_pos >= AMT_MEM) { loop_pos -= AMT_MEM;}

	return 0;
}

/**
 * JACK calls this shutdown_callback if the server ever shuts down or
 * decides to disconnect the client.
 */
void jack_shutdown (void *arg)
{
	exit (1);
}

int main (int argc, char *argv[])
{
        if (argc != 2) {
	  printf("Usage: %s mouse_dev_fname\n", argv[0]);
	  printf("Example: %s /dev/input/mouse2\n", argv[0]);
	  exit(1);
        }
	

	const char **ports;
	const char *client_name = "simple";
	const char *server_name = NULL;
	jack_options_t options = JackNullOption;
	jack_status_t status;

	/* open the mouse nonblocking.  We'll poll it each time we process a frame */
	if ((mouse_fd = open(argv[1], O_RDONLY | O_NONBLOCK)) == -1){
	  fprintf (stderr, "open mouse %s failed\n", argv[1]);
	  exit(1);
	}
	  

	/* open a client connection to the JACK server */
	client = jack_client_open (client_name, options, &status, server_name);
	if (client == NULL) {
		fprintf (stderr, "jack_client_open() failed, status = 0x%2.0x\n", status);
		if (status & JackServerFailed) { fprintf (stderr, "Unable to connect to JACK server\n"); }
		exit (1);
	}
	if (status & JackServerStarted) { fprintf (stderr, "JACK server started\n"); }
	if (status & JackNameNotUnique) {
		client_name = jack_get_client_name(client);
		fprintf (stderr, "unique name `%s' assigned\n", client_name);
	}

	/* tell the JACK server to call `process()' whenever
	   there is work to be done.
	*/
	jack_set_process_callback (client, process, 0);

	/* tell the JACK server to call `jack_shutdown()' if
	   it ever shuts down, either entirely, or if it
	   just decides to stop calling us.
	*/
	jack_on_shutdown (client, jack_shutdown, 0);

	/* display the current sample rate. */
	printf ("engine sample rate: %" PRIu32 "\n",
		jack_get_sample_rate (client));
	if (jack_get_sample_rate (client) != SAMPLE_RATE){
	  fprintf (stderr, "ERR: sample rate is not %d\n", SAMPLE_RATE);
          exit(-1);
	}
	  

	/* create input and output ports */
	input_port = jack_port_register (client, "input",
					 JACK_DEFAULT_AUDIO_TYPE,
					 JackPortIsInput, 0);
	output_port = jack_port_register (client, "output",
					  JACK_DEFAULT_AUDIO_TYPE,
					  JackPortIsOutput, 0);

	if ((input_port == NULL) || (output_port == NULL)) {
		fprintf(stderr, "no more JACK ports available\n");
		exit (1);
	}

	/* Tell the JACK server that we are ready to roll.  Our
	 * process() callback will start running now. */
	if (jack_activate (client)) {
		fprintf (stderr, "cannot activate client");
		exit (1);
	}

	/* Connect the ports.  You can't do this before the client is
	 * activated, because we can't make connections to clients
	 * that aren't running.  Note the confusing (but necessary)
	 * orientation of the driver backend ports: playback ports are
	 * "input" to the backend, and capture ports are "output" from
	 * it.
	 */
	ports = jack_get_ports (client, NULL, NULL,
				JackPortIsPhysical|JackPortIsOutput);
	if (ports == NULL) {
		fprintf(stderr, "no physical capture ports\n");
		exit (1);
	}

	if (jack_connect (client, ports[0], jack_port_name (input_port))) {
		fprintf (stderr, "cannot connect input ports\n");
	}

	free (ports);
	
	ports = jack_get_ports (client, NULL, NULL,
				JackPortIsPhysical|JackPortIsInput);
	if (ports == NULL) {
		fprintf(stderr, "no physical playback ports\n");
		exit (1);
	}

	if (jack_connect (client, jack_port_name (output_port), ports[0])) {
		fprintf (stderr, "cannot connect output ports\n");
	}

	free (ports);

	/* keep running until stopped by the user */

	sleep (-1);

	/* this is never reached but if the program
	   had some other way to exit besides being killed,
	   they would be important to call.
	*/

	jack_client_close (client);
	exit (0);
}
