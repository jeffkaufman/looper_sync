/** looper_potato
 *
 * While in looper_sync the tempo (loop length) was set by telling it
 * to make a full loop, here it's set by four "potato" taps.  The tune
 * length is then assumed to be 64 taps (beats).  We print out visual
 * indicators of where we are in the tune.
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
#define VOLUME_DECREASE 1

/* hardcoded sample rate so we can make buffer sizes depend on the
   number of samples in 60 seconds */
#define SAMPLE_RATE 48000
#define SECONDS_OF_RECORDING 60

/* how long one buffer is */
#define AMT_MEM SECONDS_OF_RECORDING*SAMPLE_RATE

/* there are three loop buffers represented in loop_bufs.  They are at
   offsets 0, AMT_MEM, and 2*AMT_MEM */
jack_default_audio_sample_t loop_bufs[AMT_MEM*3]; // three buffers

/* where in the loop buffer we're playing/recording from.  We start at
   0 when we record the first loop.*/
int loop_pos = 0;

/* where in the loop buffer to go back around to the beginning again.
   Will be a multiple of nframes 
   
   loop_end will be 64 beats times the interbeat sample length.

   Because the loop starts at 0, loop length and loop end are the
   same.
*/
int loop_end = 0;

/* tempo
 *
 * beats    64 beats    SAMPLE_RATE samples          loop          60 seconds
 * ------ = -------- *  ------------------- *  ---------------- *  ----------
 * minute     loop            second           loop_end samples      minute
 */
#define BPM(loop_e) (64*SAMPLE_RATE*60/(loop_e))

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
   P1       OFF
   P2       OFF
   P3       OFF
   RUN      any

*/

/* how long we've been on the current potato, in frames */
int potato_time;

/* how long in frames between the Nth two potatoes */
int potato_p1p2; 
int potato_p2p3; 
int potato_p3p4; 
int potato_p4p5; 

/* we're willing to wait for 3/4 of a second before deciding that the
   potatoes are to far apart */
#define TIMEOUT (SAMPLE_RATE*3/4)

#define S_OFF     0 /* nothing playing */
#define S_P1      1 /* we've gotten potato 1 */
#define S_P2      2 /* we've gotten potato 2 */
#define S_P3      3 /* we've gotten potato 3 */
#define S_P4      4 /* we've gotten potato 4 */
#define S_RUN     5 /* loop_end is set and we're away */

/* main state */
int state = S_OFF;

/* that off is 0 and rec is 1 are required by the code */
#define pS_OFF    0 /* this pedal is off */
#define pS_WREC   2 /* this pedal is waiting for the beginning of the tune to start recording */
#define pS_REC    1 /* we're recording to the buffer for this pedal */
#define pS_PLY    3 /* we're playing from the buffer for this pedal */

/* individual pedal states.  If the main state is OFF then these are
   ignored. */
int pedal_states[3];

void beep(){
  char beeparr[] = {7, '\0'};
  printf("%s", beeparr);
}

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

/* if all our pedals are off, then we're off globally too */        
void check_all_off()
{
  if (pedal_states[0] + pedal_states[1] + pedal_states[2] == pS_OFF) {
    state = S_OFF;
  }
}

void respond_to_mouse(int mouse_press, int nframes) {
  if (mouse_press == MOUSE_None) { return; }

  switch(state) {
  case S_OFF:
    printf("(potato 1)\n");
    pedal_states[0] = pS_OFF;
    pedal_states[1] = pS_OFF;
    pedal_states[2] = pS_OFF;
    potato_time = 0;
    state = S_P1;
    break;
  case S_P1:
    printf("(potato 2)\n");
    potato_p1p2 = potato_time;
    potato_time = 0;
    state = S_P2;
    break;
  case S_P2:
    printf("(potato 3)\n");
    potato_p2p3 = potato_time;
    potato_time = 0;
    state = S_P3;
    break;
  case S_P3:
    printf("(potato 4)\n");
    potato_p3p4 = potato_time;
    potato_time = 0;
    state = S_P4;
    break;
  case S_P4:
    printf("(start)\n");
    potato_p4p5 = potato_time;

    printf("potato times:\n");
    printf("  %d\n", potato_p1p2);
    printf("  %d\n", potato_p2p3);
    printf("  %d\n", potato_p3p4);
    printf("  %d\n", potato_p4p5);
    
    int avg = (potato_p1p2 + potato_p2p3 + potato_p3p4 + potato_p4p5)/4;

    printf("avg: %d\n",avg);

    avg = potato_p4p5;
    
    loop_end = avg*nframes*64; /* 64 beats to the tune */
    loop_pos = 0; /* start at the beginning of the tune */

    printf("bpm: %d\n", BPM(loop_end));

    state = S_RUN;
    pedal_states[mouse_press] = pS_WREC;

    break;
  case S_RUN:
    switch(pedal_states[mouse_press]){
    case pS_OFF:
      printf("waiting to record %d\n", mouse_press);
      pedal_states[mouse_press] = pS_WREC;
      break;
    case pS_WREC:
    case pS_REC:
    case pS_PLY:
      printf("pedal off %d\n", mouse_press);
      pedal_states[mouse_press] = pS_OFF;
      check_all_off();      
      break;
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
	respond_to_mouse(get_mouse(), nframes);

	
	for (int i = 0 ; i < nframes ; i++) {
	  out[i] = in[i] / VOLUME_DECREASE;
	}
	
	switch (state) {
	case S_OFF:
	  break;
	case S_P1:
	case S_P2:
	case S_P3:
        case S_P4:
	  potato_time++;
	  if (potato_time * nframes >= TIMEOUT) {
	    printf("potatoes timed out\n");
	    state = S_OFF;
	  }
	  break;
	case S_RUN:

	  /* print loop location */
	  if (loop_pos % (loop_end / 64) == 0) {
	    if (pedal_states[0] != pS_PLY &&
		pedal_states[1] != pS_PLY &&
		pedal_states[2] != pS_PLY){
	      /* only one is recording and the rest are off */
	      beep();
	    }
	    switch (loop_pos / (loop_end / 64)) {
	    case 0:
	      printf("A1......");
	      break;
	    case 16:
	      printf("A2......");
	      break;
	    case 32:
	      printf("B1......");
	      break;
	    case 48:
	      printf("B2......");
	      break;
	    default:
	      if ((loop_pos / (loop_end / 64)) % 4 == 0) {
		printf("........");
	      }
	      else if ((loop_pos / (loop_end / 64)) % 2 == 0) {
		printf("....    ");
	      }
	      else {
		printf(".       ");
	      }
	      break;
	    }
	    printf("              %d\n", (loop_pos / (loop_end / 64)));
	  }

	  for (int pedal = 0 ; pedal < 3 ; pedal++) {

	    if (loop_pos == 0) {
	      if (pedal_states[pedal] == pS_WREC) {
		printf ("recording secondary %d\n", pedal);
		pedal_states[pedal] = pS_REC;
	      }
	      else if (pedal_states[pedal] == pS_REC) {
		printf ("playing secondary %d\n", pedal);
                pedal_states[pedal] = pS_PLY;
              }
	    }

	    if (pedal_states[pedal] == pS_PLY) {
	      for (int i = 0 ; i < nframes ; i++) {
		out[i] += loop_bufs[AMT_MEM*pedal + loop_pos + i] / VOLUME_DECREASE;
	      }
	    }
	    else if (pedal_states[pedal] == pS_REC) {
	      for (int i = 0 ; i < nframes ; i++) {
		loop_bufs[AMT_MEM*pedal + loop_pos + i] = in[i];
	      }
	    }

	  }

	  loop_pos += nframes;
	  break;
	}
	
	if (state == S_RUN && loop_pos >= loop_end) { loop_pos = 0 ;}
	if (loop_pos >= AMT_MEM) {
	  printf("ERROR: loop_pos >= AMT_MEM %d %d\n", loop_pos, AMT_MEM);
	  loop_pos = 0;
	}

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
