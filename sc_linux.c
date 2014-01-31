/*
 * @(#)$Id: sc_linux.c,v 2.4 2009/06/26 05:18:48 baccala Exp $
 *
 * Copyright (C) 1996 - 2001 Tim Witham <twitham@quiknet.com>
 *
 * (see the files README and COPYING for more details)
 *
 * This file implements the Linux ESD and sound card interfaces
 *
 * These two interfaces are very similar and share a lot of code.  In
 * shared routines, we can tell which one we're using by looking at
 * the 'snd' and 'esd' variables to see which one is a valid file
 * descriptor (!= -1).  Although it's possible for both to be open at
 * the same time (say, when sound card is open, user pushes '&' to
 * select next device, and xoscope tries to open ESD to see if it
 * works before closing the sound card), this really shouldn't happen
 * in any of the important routines.
 *
 */

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>		/* for abs() */
#include <sys/ioctl.h>
#include <sys/soundcard.h>
#include "oscope.h"		/* program defaults */
#ifdef HAVE_LIBESD
#include <esd.h>
#endif
#ifdef HAVE_ASOUND
#include <alsa/asoundlib.h>
#endif

#define ESDDEVICE "ESounD"
#define SOUNDDEVICE "/dev/dsp"
#define ASOUNDDEV   "hw:0,0"

static int snd = -2;		/* file descriptor for /dev/dsp */

#ifdef HAVE_LIBESD
static int esdblock = 0;	/* 1 to block ESD; 0 to non-block */
static int esdrecord = 0;	/* 1 to use ESD record mode; 0 to use ESD monitor mode */
static int esd = -2;		/* file descriptor for ESD */
#endif

#ifdef HAVE_ASOUND
snd_pcm_t* pcm = NULL;
snd_pcm_hw_params_t* params = NULL;
snd_pcm_uframes_t pcm_frames = 32;
struct pollfd* pcm_fds = NULL;
int n_pcm_fds = 0;
int a_pcm_fds = 0;
#endif


static int sc_chans = 0;
static int sound_card_rate = DEF_R;	/* sampling rate of sound card */

/* Signal structures we're capturing into */
static Signal left_sig = {"Left Mix", "a"};
static Signal right_sig = {"Right Mix", "b"};

static int trigmode = 0;
static int triglev;
static int trigch;

static char * snd_errormsg1 = NULL;
static char * snd_errormsg2 = NULL;

#ifdef HAVE_LIBESD
static char * esd_errormsg1 = NULL;
static char * esd_errormsg2 = NULL;
#endif

#ifdef HAVE_ASOUND
static char * asound_errormsg1 = NULL;
static char * asound_errormsg2 = NULL;
#endif

/* This function is defined as do-nothing and weak, meaning it can be
 * overridden by the linker without error.  It's used for the X
 * Windows GUI for this data source, and is defined in this way so
 * that this object file can be used either with or without GTK.  If
 * this causes compiler problems, just comment out the attribute lines
 * and leave the do-nothing functions.
 */

void sc_gtk_option_dialog() __attribute__ ((weak));
void sc_gtk_option_dialog() {}

void esd_gtk_option_dialog() __attribute__ ((weak));
void esdsc_gtk_option_dialog() {}

/* close the sound device */
static void
close_sound_card()
{
  if (snd >= 0) {
    close(snd);
    snd = -2;
  }
}

/* show system error and close sound device if given ioctl status is bad */
static void
check_status_ioctl(int d, int request, void *argp, int line)
{
  if (ioctl(d, request, argp) < 0) {
    snd_errormsg1 = "sound ioctl";
    snd_errormsg2 = strerror(errno);
    close_sound_card();
  }
}

#ifdef HAVE_LIBESD

/* close ESD */
static void
close_ESD()
{
  if (esd >= 0) {
    close(esd);
    esd = -2;
  }
}

/* turn ESD on */
static int
open_ESD(void)
{
  if (esd >= 0) return 1;

  esd_errormsg1 = NULL;
  esd_errormsg2 = NULL;

  if (esdrecord) {
    esd = esd_record_stream_fallback(ESD_BITS8|ESD_STEREO|ESD_STREAM|ESD_RECORD,
				     ESD_DEFAULT_RATE, NULL, progname);
  } else {
    esd = esd_monitor_stream(ESD_BITS8|ESD_STEREO|ESD_STREAM|ESD_MONITOR,
                             ESD_DEFAULT_RATE, NULL, progname);
  }

  if (esd <= 0) {
    esd_errormsg1 = "opening " ESDDEVICE;
    esd_errormsg2 = strerror(errno);
    return 0;
  }

  /* we have esd connection! non-block it? */
  if (!esdblock)
    fcntl(esd, F_SETFL, O_NONBLOCK);

  return 1;
}

#endif

#ifdef HAVE_ASOUND
static void close_asound()
{
    if (pcm != NULL) {
	snd_pcm_close(pcm);
	pcm = NULL;
    }
    if (params != NULL) {
	// snd_pcm_hw_params_free(params);
	params = NULL;
    }
}

static int open_asound(void)
{
    int rc;
    int chans = 2;
    unsigned int val;
    unsigned int rate;
    snd_pcm_format_t format;

    if (pcm != NULL)
	return 1;
    rc = snd_pcm_open(&pcm, ASOUNDDEV, SND_PCM_STREAM_CAPTURE, 
		      SND_PCM_NONBLOCK);
    fprintf(stderr, "ALSA open = %d, pcm=%p\n", rc, pcm);
    if (rc < 0) {
	asound_errormsg1 = "opening " ASOUNDDEV;
	asound_errormsg2 = (char*) snd_strerror(rc);
	return 0;
    }
    snd_pcm_hw_params_alloca(&params);    // allocate hardware params
    snd_pcm_hw_params_any(pcm, params);   // fill with defaults

    snd_pcm_hw_params_set_rate_resample(pcm, params, 1);

    // read default values and print them
    snd_pcm_hw_params_get_channels(params, &val);
    fprintf(stderr, "default channels = %d\n", val);
    snd_pcm_hw_params_get_format(params, &format);
    fprintf(stderr, "default format = %s\n", snd_pcm_format_name(format));
    snd_pcm_hw_params_get_rate(params, &rate, 0);
    fprintf(stderr, "default rate = %u\n", rate);

    snd_pcm_hw_params_set_access(pcm, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm, params, SND_PCM_FORMAT_S8); // S16_LE);

    sc_chans = chans;
    snd_pcm_hw_params_set_channels(pcm, params, chans);

    sound_card_rate = 8000; // 44100; // 8000;
    val = sound_card_rate;
    snd_pcm_hw_params_set_rate_near(pcm, params, &val, 0);
    snd_pcm_hw_params_set_period_size_near(pcm, params, &pcm_frames, 0);

    snd_pcm_hw_params_get_channels(params, &val);
    fprintf(stderr, "channels = %d\n", val);
    snd_pcm_hw_params_get_format(params, &format);
    fprintf(stderr, "format = %s\n", snd_pcm_format_name(format));
    snd_pcm_hw_params_get_rate(params, &rate, 0);
    fprintf(stderr, "rate = %u\n", rate);

    rc = snd_pcm_hw_params(pcm, params);
    // rc = 0;
    fprintf(stderr, "ALSO open = %d\n", rc);
    if (rc < 0) {
	asound_errormsg1 = "set hw " ASOUNDDEV;
	asound_errormsg2 = (char*) snd_strerror(rc);
	snd_pcm_close(pcm);
	return 0;
    }

    return 1;
}
#endif

/* turn the sound device (/dev/dsp) on */
static int
open_sound_card(void)
{
  int rate = sound_card_rate;
  int chan = 2;
  int bits = 8;
  int parm;
  int i = 3;

  if (snd >= 0) return 1;

  snd_errormsg1 = NULL;
  snd_errormsg2 = NULL;

  /* we try a few times in case someone else is using device (FvwmAudio) */
  while ((snd = open(SOUNDDEVICE, O_RDONLY | O_NDELAY)) < 0 && i > 0) {
    sleep(1);
    i --;
  }

  if (snd < 0) {
    snd_errormsg1 = "opening " SOUNDDEVICE;
    snd_errormsg2 = strerror(errno);
    return 0;
  }

  parm = chan;			/* set mono/stereo */
  //check_status_ioctl(snd, SOUND_PCM_WRITE_CHANNELS, &parm, __LINE__);
  check_status_ioctl(snd, SNDCTL_DSP_CHANNELS, &parm, __LINE__);
  sc_chans = parm;

  parm = bits;			/* set 8-bit samples */
  check_status_ioctl(snd, SOUND_PCM_WRITE_BITS, &parm, __LINE__);

  parm = rate;			/* set sampling rate */
  //check_status_ioctl(snd, SOUND_PCM_WRITE_RATE, &parm, __LINE__);
  //check_status_ioctl(snd, SOUND_PCM_READ_RATE, &parm, __LINE__);
  check_status_ioctl(snd, SNDCTL_DSP_SPEED, &parm, __LINE__);
  sound_card_rate = parm;

  return 1;
}

static int
reset_sound_card(void)
{
    static char junk[SAMPLESKIP];

#ifdef HAVE_LIBESD
    if (esd >= 0) {
	close_ESD();
	open_ESD();
	if (esd < 0) return;
	return read(esd, junk, SAMPLESKIP);
    }
#endif

#ifdef HAVE_ASOUND
    if (pcm != NULL) {
	close_asound();
	open_asound();
	// if (pcm != NULL) snd_pcm_drain(pcm);
	fprintf(stderr, "ALSO reset done\n");
	return 0;
    }
#endif

    if (snd >= 0) {
	close_sound_card();
	open_sound_card();
	if (snd < 0) return 0;
	return read(snd, junk, SAMPLESKIP);
    }
    return 0;
}

#ifdef HAVE_LIBESD

static int esd_nchans(void)
{
  /* ESD always has two channels, right? */

  if (esd == -2) open_ESD();

  return (esd >= 0) ? 2 : 0;
}

static int esd_fd(void)
{
    return esd;
}

#endif


#ifdef HAVE_ASOUND

static int asound_nchans(void)
{
    if (pcm == NULL) open_asound();
    return (pcm != NULL) ? sc_chans : 0;
}

static int asound_fd(void)
{
    if (pcm != NULL) {
	int n = snd_pcm_poll_descriptors_count(pcm);
	if (n > a_pcm_fds) {
	    pcm_fds = realloc(pcm_fds, n*sizeof(struct pollfd));
	    a_pcm_fds = n;
	}
	printf("#pollfd = %d\n", n);
	if ((n_pcm_fds = n) > 0) {
	    if (snd_pcm_poll_descriptors(pcm, pcm_fds, n_pcm_fds) < 0)
		return -1;
	    printf("#fd[0] = %d\n", pcm_fds[0].fd);
	    return pcm_fds[0].fd;
	}
    }
    return -1;
}

#endif

static int sc_nchans(void)
{
  if (snd == -2) open_sound_card();
  return (snd >= 0) ? sc_chans : 0;
}

static int sc_fd(void)
{
    return snd;
}

static Signal *sc_chan(int chan)
{
  return (chan ? &right_sig : &left_sig);
}

/* Triggering - we save the trigger level in the raw, unsigned
 * byte values that we read from the sound card
 */

static int set_trigger(int chan, int *levelp, int mode)
{
  trigch = chan;
  trigmode = mode;
  triglev = 127 + *levelp;
  if (triglev > 255) {
    triglev = 255;
    *levelp = 128;
  }
  if (triglev < 0) {
    triglev = 0;
    *levelp = -128;
  }
  return 1;
}

static void clear_trigger(void)
{
  trigmode = 0;
}

static int change_rate(int dir)
{
  int newrate = sound_card_rate;

  if (dir > 0) {
    if (sound_card_rate > 16500)
      newrate = 44100;
    else if (sound_card_rate > 9500)
      newrate = 22050;
    else
      newrate = 11025;
  } else {
    if (sound_card_rate < 16500)
      newrate = 8000;
    else if (sound_card_rate < 33000)
      newrate = 11025;
    else
      newrate = 22050;
  }

  if (newrate != sound_card_rate) {
    sound_card_rate = newrate;
    return 1;
  }
  return 0;
}

#ifdef HAVE_LIBESD
static int esd_change_rate(int dir)
{
    return 0;
}
#endif

static void
reset(void)
{
    reset_sound_card();

#ifdef HAVE_LIBESD
  left_sig.rate = esd >= 0 ? ESD_DEFAULT_RATE : sound_card_rate;
  right_sig.rate = esd >= 0 ? ESD_DEFAULT_RATE : sound_card_rate;
#else
  left_sig.rate = sound_card_rate;
  right_sig.rate = sound_card_rate;
#endif

  left_sig.num = 0;
  left_sig.frame ++;

  right_sig.num = 0;
  right_sig.frame ++;
}

/* set_width(int)
 *
 * sets the frame width (number of samples captured per sweep) globally
 * for all the channels.
 */

static void set_width(int width)
{
  left_sig.width = width;
  right_sig.width = width;

  if (left_sig.data != NULL) free(left_sig.data);
  if (right_sig.data != NULL) free(right_sig.data);

  left_sig.data = malloc(width * sizeof(short));
  right_sig.data = malloc(width * sizeof(short));
}

/*
 * check for trigger in among interleaved channels
 */
static int trigger_rising(unsigned char* buffer, int ns, 
			  int nchannels, int chan, int level)
{
    int n = nchannels*ns;
    int i = 0;
    while(i < n) {
	int j = i + nchannels;
	if ((buffer[i+chan] < level) && (buffer[j+chan] >= level))
	    return i;
	i = j;
    }
    return -1;
}

static int trigger_falling(unsigned char* buffer, int ns, 
			   int nchannels, int chan, int level)
{
    int n = nchannels*ns;
    int i = 0;
    while(i < n) {
	int j = i + nchannels;
	if ((buffer[i+chan] >= level) && (buffer[j+chan] < level))
	    return i;
	i = j;
    }
    return -1;
}

static int trigger_both(unsigned char* buffer, int ns, 
			int nchannels, int chan, int level)
{
    int n = nchannels*ns;
    int i = 0;
    while(i <= n) {
	int j = i + nchannels;
	if (((buffer[i+chan] >= level) && (buffer[j+chan] < level)) ||
	    ((buffer[i+chan] <  level) && (buffer[j+chan] >= level)))
	    return i;
	i = j;
    }
    return -1;
}

/*
 * Common data load function 
 */
static int load_data(unsigned char* buffer, int ns, int nchannels,
		     int* nump, int width)
{
    int n = nchannels*ns;
    int i = 0;
    int j = *nump;

    switch(nchannels) {
    case 0:
	break;
    case 1:
	while((i < n) && (j < width)) {
	    left_sig.data[j] = buffer[i] - 127;
	    right_sig.data[j] = 0;
	    i++;
	    j++;
	}
	break;
    case 2:
	while((i < n) && (j < width)) {
	    left_sig.data[j]  = buffer[i] - 127;
	    right_sig.data[j] = buffer[i+1] - 127;
	    i += 2;
	    j++;
	}
	break;
    default:
	break;
    }
    *nump = j;
    left_sig.num = j;
    right_sig.num = j;
    if (j >= width) *nump = 0;
    return 1;
}

static int do_get_data(unsigned char* ptr, int ns)
{
    if (!in_progress) {
	int delay = 0;
	int i;
	switch(trigmode) {
	case 1:
	    if ((i=trigger_falling(ptr,ns,sc_chans,trigch,triglev)) < 0)
		return 0;
	    break;
	case 2:
	    if ((i=trigger_rising(ptr,ns,sc_chans,trigch,triglev)) < 0)
		return 0;
	    break;
	case 3:
	    if ((i = trigger_both(ptr,ns,sc_chans,trigch,triglev)) < 0)
		return 0;
	    break;
	default:
	    i = 0;
	    break;
	}
	ptr += i;
	ns  -= i;

	if (trigmode) {
	    // not 100% correct! fixme use real dubble buffer
	    int a = ptr[trigch] - 127;
	    int b = ptr[sc_chans+trigch] - 127;
	    if (a != b) // ????
		delay = abs(10000*(b-(triglev-127)) / (b-a));
	}

	switch(sc_chans) {
	case 1:
	    left_sig.data[0] = ptr[0] - 127;
	    left_sig.delay = delay;
	    left_sig.num = 1;
	    left_sig.frame ++;
		
	    right_sig.data[0] = 0;
	    right_sig.delay = delay;
	    right_sig.num = 1;
	    right_sig.frame ++;
	    ptr += 1;
	    ns--;
	    break;
	case 2:
	    left_sig.data[0] = ptr[0] - 127;
	    left_sig.delay = delay;
	    left_sig.num = 1;
	    left_sig.frame ++;
		
	    right_sig.data[0] = ptr[1] - 127;
	    right_sig.delay = delay;
	    right_sig.num = 1;
	    right_sig.frame ++;
	    ptr += 2;
	    ns--;	    
	    break;
	default:
	    break;
	}
	printf("frame=%d\n", left_sig.frame);
	in_progress = 1;
    }
    return load_data(ptr, ns, sc_chans, &in_progress, left_sig.width);
}

#ifdef HAVE_ASOUND
/* get data from sound card, return value is whether we triggered or not */
static int asound_get_data()
{
    static unsigned char buffer[MAXWID * 2];
    int ns;
    unsigned short revents;
    
    // assert that sizeof(buffer) > pcm_frames*sc_chans*1 

    if (pcm == NULL) return 0;

    snd_pcm_poll_descriptors_revents(pcm, pcm_fds, n_pcm_fds, &revents);
    if (!(revents & POLLIN)) {
	printf("no poll\n");
	return 1;
	// return in_progress;
    }
    if (!in_progress)
	snd_pcm_drain(pcm);
    ns = snd_pcm_readi(pcm, buffer, pcm_frames);

    // fixme: this is just for testing
    if (ns < 0) {
	if (errno == EAGAIN)
	    return 1;
	return 0;
    }
    return do_get_data(buffer, ns);
}
#endif

#ifdef HAVE_LIBESD
/* get data from sound card, return value is whether we triggered or not */
static int get_data()
{
    static unsigned char buffer[MAXWID * 2];
    int ns;

    if (esd < 0) 
	return 0;
    if (!in_progress) {
	/* Discard excess samples so we can keep our time snapshot close
	   to real-time and minimize sound recording overruns.  For ESD we
	   don't know how many are available (do we?) so we discard them
	   all to start with a fresh buffer that hopefully won't wrap
	   around before we get it read. */
	
	/* read until we get something smaller than a full buffer */
	while ((ns = read(esd, buffer, sizeof(buffer))) == sizeof(buffer));
    } else {
	ns = read(esd, buffer, sizeof(buffer));
    }
    ns = (ns < 0) ? 0 : (ns / sc_chans);
    return do_get_data(buffer, ns);
}
#endif

/* get data from sound card, return value is whether we triggered or not */
static int sc_get_data()
{
    static unsigned char buffer[MAXWID * 2];
    int ns;

    if (snd < 0)
	return 0;
    if (!in_progress) {
	/* Discard excess samples so we can keep our time snapshot close
	   to real-time and minimize sound recording overruns.  For ESD we
	   don't know how many are available (do we?) so we discard them
	   all to start with a fresh buffer that hopefully won't wrap
	   around before we get it read. */
	
	/* read until we get something smaller than a full buffer */
	while ((ns = read(snd, buffer, sizeof(buffer))) == sizeof(buffer));
    } else {
	ns = read(snd, buffer, sizeof(buffer));
    }
    ns = (ns < 0) ? 0 : (ns / sc_chans);
    return do_get_data(buffer, ns);
}



static char * snd_status_str(int i)
{
#ifdef DEBUG
  static char string[16];
  sprintf(string, "status %d", i);
  return string;
#endif

  switch (i) {
  case 0:
    if (snd_errormsg1) return snd_errormsg1;
    else return "";
  case 2:
    if (snd_errormsg2) return snd_errormsg2;
    else return "";
  }
  return NULL;
}

#ifdef HAVE_LIBESD

static char * esd_status_str(int i)
{
  switch (i) {
  case 0:
    if (esd_errormsg1) return esd_errormsg1;
    else return "";
  case 2:
    if (esd_errormsg2) return esd_errormsg2;
    else return "";
  }
  return NULL;
}

/* ESD option key 1 (*) - toggle Record mode */

static int option1_esd(void)
{
  if(esdrecord)
    esdrecord = 0;
  else
    esdrecord = 1;

  return 1;
}

static char * option1str_esd(void)
{
  static char string[16];

  sprintf(string, "RECORD:%d", esdrecord);
  return string;
}

static int esd_set_option(char *option)
{
  if (sscanf(option, "esdrecord=%d", &esdrecord) == 1) {
    return 1;
  } else {
    return 0;
  }
}

/* The GUI interface in sc_linux_gtk.c depends on the esdrecord option
 * being number 1.
 */

static char * esd_save_option(int i)
{
  static char buf[32];

  switch (i) {
  case 1:
    snprintf(buf, sizeof(buf), "esdrecord=%d", esdrecord);
    return buf;

  default:
    return NULL;
  }
}

#endif

#ifdef DEBUG
static char * option1str_sc(void)
{
  static char string[16];

  sprintf(string, "opt1");
  return string;
}

static char * option2str_sc(void)
{
  static char string[16];

  sprintf(string, "opt2");
  return string;
}
#endif

static int sc_set_option(char *option)
{
  if (sscanf(option, "rate=%d", &sound_card_rate) == 1) {
    return 1;
  } else if (strcmp(option, "dma=") == 0) {
    /* a deprecated option, return 1 so we don't indicate error */
    return 1;
  } else {
    return 0;
  }
}

static char * sc_save_option(int i)
{
  static char buf[32];

  switch (i) {
  case 0:
    snprintf(buf, sizeof(buf), "rate=%d", sound_card_rate);
    return buf;

  default:
    return NULL;
  }
}

#ifdef HAVE_ASOUND

static char * asound_status_str(int i)
{
  switch (i) {
  case 0:
    if (asound_errormsg1) return asound_errormsg1;
    else return "";
  case 2:
    if (asound_errormsg2) return asound_errormsg2;
    else return "";
  }
  return NULL;
}
#endif

#ifdef HAVE_ASOUND
DataSrc datasrc_asound = {
  "ALSA",
  asound_nchans,
  sc_chan,
  set_trigger,
  clear_trigger,
  change_rate,
  set_width,
  reset,
  asound_fd,
  asound_get_data,
  asound_status_str,
  NULL,  /* option1, */
  NULL,  /* option1str, */
  NULL,  /* option2, */
  NULL,  /* option2str, */
  sc_set_option,
  sc_save_option,
  NULL,  /* gtk_options */
};
#endif



#ifdef HAVE_LIBESD
DataSrc datasrc_esd = {
  "ESD",
  esd_nchans,
  sc_chan,
  set_trigger,
  clear_trigger,
  esd_change_rate,
  set_width,
  reset,
  esd_fd,
  esd_get_data,
  esd_status_str,
  option1_esd,  /* option1 */
  option1str_esd,  /* option1str */
  NULL,  /* option2, */
  NULL,  /* option2str, */
  esd_set_option,  /* set_option */
  esd_save_option,  /* save_option */
  esd_gtk_option_dialog,  /* gtk_options */
};
#endif

DataSrc datasrc_sc = {
  "Soundcard",
  sc_nchans,
  sc_chan,
  set_trigger,
  clear_trigger,
  change_rate,
  set_width,
  reset,
  sc_fd,
  sc_get_data,
  snd_status_str,
#ifdef DEBUG
  NULL,
  option1str_sc,
  NULL,
  option2str_sc,
#else
  NULL,  /* option1, */
  NULL,  /* option1str, */
  NULL,  /* option2, */
  NULL,  /* option2str, */
#endif
  sc_set_option,
  sc_save_option,
  NULL,  /* gtk_options */
};
