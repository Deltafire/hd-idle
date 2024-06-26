/*
 * hd-idle.c - external disk idle daemon
 *
 * Copyright (c) 2007 Christian Mueller.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <stdarg.h>

#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <scsi/sg.h>
#include <scsi/scsi.h>

#define STAT_FILE "/proc/diskstats"
#define DEFAULT_IDLE_TIME 600

#define dprintf if (debug) printf

/* typedefs and structures */
typedef struct IDLE_TIME {
  struct IDLE_TIME  *next;
  char              *name;
  int                idle_time;
} IDLE_TIME;

typedef struct DISKSTATS {
  struct DISKSTATS  *next;
  char               name[50];
  int                idle_time;
  time_t             last_io;
  time_t             spindown;
  time_t             spinup;
  unsigned int       spun_down : 1;
  unsigned int       reads;
  unsigned int       writes;
} DISKSTATS;

/* function prototypes */
static void        daemonize       (void);
static DISKSTATS  *get_diskstats   (const char *name);
static void        spindown_disk   (const char *name);
static void        log_spinup      (DISKSTATS *ds);
static void        log_remonitor   ();
static void        log_mondisk     (DISKSTATS *ds);
static char       *disk_name       (char *name);
static void        phex            (const void *p, int len,
                                    const char *fmt, ...);

/* global/static variables */
IDLE_TIME *it_root;
DISKSTATS *ds_root;
char *logfile = "/dev/null";
int debug, foreground;

/* main function */
int main(int argc, char *argv[])
{
  IDLE_TIME *it;
  int have_logfile = 0;
  int min_idle_time;
  int sleep_time;
  int skew_time; 
  int opt;
  time_t now;
  time_t lastnow;

  /* create default idle-time parameter entry */
  if ((it = malloc(sizeof(*it))) == NULL) {
    fprintf(stderr, "out of memory\n");
    exit(1);
  }
  it->next = NULL;
  it->name = NULL;
  it->idle_time = DEFAULT_IDLE_TIME;
  it_root = it;

  /* process command line options */
  while ((opt = getopt(argc, argv, "t:a:i:l:dfh")) != -1) {
    switch (opt) {

    case 't':
      /* just spin-down the specified disk and exit */
      spindown_disk(optarg);
      return(0);

    case 'a':
      /* add a new set of idle-time parameters for this particular disk */
      if ((it = malloc(sizeof(*it))) == NULL) {
        fprintf(stderr, "out of memory\n");
        return(2);
      }
      it->name = disk_name(optarg);
      it->idle_time = DEFAULT_IDLE_TIME;
      it->next = it_root;
      it_root = it;
      break;

    case 'i':
      /* set idle-time parameters for current (or default) disk */
      it->idle_time = atoi(optarg);
      break;

    case 'l':
      logfile = optarg;
      have_logfile = 1;
      break;

    case 'd':
      debug = 1;
      break;

    case 'f':
      foreground = 1;
      break;

    case 'h':
      printf("usage: hd-idle [-t <disk>] [-a <name>] [-i <idle_time>] [-l <logfile>] [-d] [-f] [-h]\n");
      return(0);

    case ':':
      fprintf(stderr, "error: option -%c requires an argument\n", optopt);
      return(1);

    case '?':
      fprintf(stderr, "error: unknown option -%c\n", optopt);
      return(1);
    }
  }

  /* set sleep time to 1/10th of the shortest idle time */
  min_idle_time = 1 << 30;
  for (it = it_root; it != NULL; it = it->next) {
    if (it->idle_time != 0 && it->idle_time < min_idle_time) {
      min_idle_time = it->idle_time;
    }
  }
  if ((sleep_time = min_idle_time / 10) == 0) {
    sleep_time = 1;
  }

  /* set skew time between scans as a multiple of sleep_time */
  skew_time = sleep_time * 3;

  /* daemonize unless we're running in debug or foreground mode */
  if (!debug && !foreground) {
    daemonize();
  }

  /* main loop: probe for idle disks and stop them */
  lastnow = time(NULL);

  for (;;) {
    DISKSTATS tmp;
    FILE *fp;
    char buf[200];

    if ((fp = fopen(STAT_FILE, "r")) == NULL) {
      perror(STAT_FILE);
      return(2);
    }

    memset(&tmp, 0x00, sizeof(tmp));

    now = time(NULL);
    if (now - lastnow > skew_time) {
      /* we slept too long, assume a suspend event and disks may be spun up */
      for (it = it_root; it != NULL; it = it->next) {
        /* reset spin status and timers */
        if (it->name != NULL) {
          DISKSTATS *ds;
          ds = get_diskstats(it->name);
          ds->spinup = now;
          ds->last_io = now;
          ds->spun_down = 0;
          log_mondisk(ds);
        }	
      }
      log_remonitor();
    }

    while (fgets(buf, sizeof(buf), fp) != NULL) {
      if (sscanf(buf, "%*d %*d %s %*u %*u %u %*u %*u %*u %u %*u %*u %*u %*u",
                 tmp.name, &tmp.reads, &tmp.writes) == 3) {
        DISKSTATS *ds;

        now = time(NULL);

        const char *s;

        /* make sure this is a SCSI disk (sd[a-z]+) without partition number */
        if (tmp.name[0] != 's' || tmp.name[1] != 'd') {
          continue;
        }
        for (s = tmp.name + 2; isalpha(*s); s++);
        if (*s != '\0') {
          /* ignore disk partitions */ 
          continue;
        }

        dprintf("probing %s: reads: %u, writes: %u\n", tmp.name, tmp.reads, tmp.writes);

        /* get previous statistics for this disk */
        ds = get_diskstats(tmp.name);

        if (ds == NULL) {
          /* new disk; just add it to the linked list */
          if ((ds = malloc(sizeof(*ds))) == NULL) {
            fprintf(stderr, "out of memory\n");
            return(2);
          }
          memcpy(ds, &tmp, sizeof(*ds));
          ds->last_io = now;
          ds->spinup = ds->last_io;
          ds->next = ds_root;
          ds_root = ds;

          /* find idle time for this disk (falling-back to default; default means
           * 'it->name == NULL' and this entry will always be the last due to the
           * way this single-linked list is built when parsing command line
           * arguments)
           */
          for (it = it_root; it != NULL; it = it->next) {
            if (it->name == NULL || !strcmp(ds->name, it->name)) {
              ds->idle_time = it->idle_time;
              break;
            }
          }
	  log_mondisk(ds);

        } else if (ds->reads == tmp.reads && ds->writes == tmp.writes) {
          if (!ds->spun_down) {
            /* no activity on this disk and still running */
            if (ds->idle_time != 0 && now - ds->last_io >= ds->idle_time) {
              spindown_disk(ds->name);
              ds->spindown = now;
              ds->spun_down = 1;
            }
          }

        } else {
          /* disk had some activity */
          if (ds->spun_down) {
            /* disk was spun down, thus it has just spun up */
            if (have_logfile) {
              log_spinup(ds);
            }
            ds->spinup = now;
          }
          ds->reads = tmp.reads;
          ds->writes = tmp.writes;
          ds->last_io = now;
          ds->spun_down = 0;
        }
      }
    }

    lastnow = now;
    fclose(fp);
    sleep(sleep_time);
  }

  return(0);
}

/* become a daemon */
static void daemonize(void)
{
  int maxfd;
  int i;

  /* fork #1: exit parent process and continue in the background */
  if ((i = fork()) < 0) {
    perror("couldn't fork");
    exit(2);
  } else if (i > 0) {
    _exit(0);
  }

  /* fork #2: detach from terminal and fork again so we can never regain
   * access to the terminal */
  setsid();
  if ((i = fork()) < 0) {
    perror("couldn't fork #2");
    exit(2);
  } else if (i > 0) {
    _exit(0);
  }

  /* change to root directory and close file descriptors */
  chdir("/");
  maxfd = getdtablesize();
  for (i = 0; i < maxfd; i++) {
    close(i);
  }

  /* use /dev/null for stdin, stdout and stderr */
  open("/dev/null", O_RDONLY);
  open("/dev/null", O_WRONLY);
  open("/dev/null", O_WRONLY);
}

/* get DISKSTATS entry by name of disk */
static DISKSTATS *get_diskstats(const char *name)
{
  DISKSTATS *ds;

  for (ds = ds_root; ds != NULL; ds = ds->next) {
    if (!strcmp(ds->name, name)) {
      return(ds);
    }
  }

  return(NULL);
}

/* spin-down a disk */
static void spindown_disk(const char *name)
{
  struct sg_io_hdr io_hdr;
  unsigned char sense_buf[255];
  char dev_name[100];
  int fd;

  dprintf("spindown: %s\n", name);

  /* fabricate SCSI IO request */
  memset(&io_hdr, 0x00, sizeof(io_hdr));
  io_hdr.interface_id = 'S';
  io_hdr.dxfer_direction = SG_DXFER_NONE;

  /* SCSI stop unit command */
  io_hdr.cmdp = (unsigned char *) "\x1b\x00\x00\x00\x00\x00";

  io_hdr.cmd_len = 6;
  io_hdr.sbp = sense_buf;
  io_hdr.mx_sb_len = (unsigned char) sizeof(sense_buf);

  /* open disk device (kernel 2.4 will probably need "sg" names here) */
  snprintf(dev_name, sizeof(dev_name), "/dev/%s", name);
  if ((fd = open(dev_name, O_RDONLY)) < 0) {
    perror(dev_name);
    return;
  }

  /* execute SCSI request */
  if (ioctl(fd, SG_IO, &io_hdr) < 0) {
    char buf[100];
    snprintf(buf, sizeof(buf), "ioctl on %s:", name);
    perror(buf);

  } else if (io_hdr.masked_status != 0) {
    fprintf(stderr, "error: SCSI command failed with status 0x%02x\n",
            io_hdr.masked_status);
    if (io_hdr.masked_status == CHECK_CONDITION) {
      phex(sense_buf, io_hdr.sb_len_wr, "sense buffer:\n");
    }
  }

  close(fd);
}

/* write a spin-up event message to the log file */
static void log_spinup(DISKSTATS *ds)
{
  FILE *fp;

  if ((fp = fopen(logfile, "a")) != NULL) {
    /* Print statistics to logfile
     *
     * Note: This doesn't work too well if there are multiple disks
     *       because the I/O we're dealing with might be on another
     *       disk so we effectively wake up the disk the log file is
     *       stored on as well. Then again the logfile is a debugging
     *       option, so what...
     */
    time_t now = time(NULL);
    char tstr[20];
    char dstr[20];

    strftime(dstr, sizeof(dstr), "%Y-%m-%d", localtime(&now));
    strftime(tstr, sizeof(tstr), "%H:%M:%S", localtime(&now));
    fprintf(fp,
            "date: %s, time: %s, disk: %s, running: %ld, stopped: %ld\n",
            dstr, tstr, ds->name,
            (long) ds->spindown - (long) ds->spinup,
            (long) time(NULL) - (long) ds->spindown);

    /* Sync to make sure writing to the logfile won't cause another
     * spinup in 30 seconds (or whatever bdflush uses as flush interval).
     */
    fclose(fp);
    sleep(1);
    sync();
  }
}

/* write a drive spin monitor reset message to the log file */
static void log_remonitor()
{
  FILE *fp;
  
  if ((fp = fopen(logfile, "a")) != NULL) {
    /* Print wakeup to the logfile
     */
    time_t now = time(NULL);
    char tstr[20];
    char dstr[20];

    strftime(dstr, sizeof(dstr), "%Y-%m-%d", localtime(&now));
    strftime(tstr, sizeof(tstr), "%H:%M:%S", localtime(&now));
    fprintf(fp,
            "date: %s, time: %s: assuming disks spun up after long sleep\n",
            dstr, tstr);
    /* Sync to make sure writing to the logfile won't case another
     * spinup
     */
    fclose(fp);
    sleep(1);
    sync();
  }
}

/* write a drive monitoring message when a new disk is discovered */
static void log_mondisk(DISKSTATS *ds)
{
  FILE *fp;
  if ((fp = fopen(logfile, "a")) != NULL) {
    /* Print disk statistic to logfile */
    time_t now = time(NULL);
    char tstr[20];
    char dstr[20];

    strftime(dstr, sizeof(dstr), "%Y-%m-%d", localtime(&now));
    strftime(tstr, sizeof(tstr), "%H:%M:%S", localtime(&now));
    fprintf(fp,
            "date: %s, time: %s, disk: %s, monitoring started with idle timout: %ld\n",
            dstr, tstr, ds->name, (long) ds->idle_time);
    fclose(fp);
    sync(); /* sync without sleep */
  }
}

/* Resolve disk names specified as "/dev/disk/by-xxx" or some other symlink.
 * Please note that this function is only called during command line parsing
 * and hd-idle per se does not support dynamic disk additions or removals at
 * runtime.
 *
 * This might change in the future but would require some fiddling to avoid
 * needless overhead -- after all, this was designed to run on tiny embedded
 * devices, too.
 */
static char *disk_name(char *path)
{
  ssize_t len;
  char buf[256];
  char *s;

  if (*path != '/') {
    /* just a disk name without /dev prefix */
    return(path);
  }

  if ((len = readlink(path, buf, sizeof(buf) - 1)) <= 0) {
    if (errno != EINVAL) {
      /* couldn't resolve disk name */
      return(path);
    }

    /* 'path' is not a symlink */
    strncpy(buf, path, sizeof(buf) - 1);
    buf[sizeof(buf)-1] = '\0';
    len = strlen(buf);
  }
  buf[len] = '\0';

  /* remove partition numbers, if any */
  for (s = buf + strlen(buf) - 1; s >= buf && isdigit(*s); s--) {
    *s = '\0';
  }

  /* Extract basename of the disk in /dev. Note that this assumes that the
   * final target of the symlink (if any) resolves to /dev/sd*
   */
  if ((s = strrchr(buf, '/')) != NULL) {
    s++;
  } else {
    s = buf;
  }

  if ((s = strdup(s)) == NULL) {
    fprintf(stderr, "out of memory");
    exit(2);
  }

  if (debug) {
    printf("using %s for %s\n", s, path);
  }
  return(s);
}

/* print hex dump to stderr (e.g. sense buffers) */
static void phex(const void *p, int len, const char *fmt, ...)
{
  va_list va;
  const unsigned char *buf = p;
  int pos = 0;
  int i;

  /* print header */
  va_start(va, fmt);
  vfprintf(stderr, fmt, va);

  /* print hex block */
  while (len > 0) {
    fprintf(stderr, "%08x ", pos);

    /* print hex block */
    for (i = 0; i < 16; i++) {
      if (i < len) {
        fprintf(stderr, "%c%02x", ((i == 8) ? '-' : ' '), buf[i]);
      } else {
        fprintf(stderr, "   ");
      }
    }

    /* print ASCII block */
    fprintf(stderr, "   ");
    for (i = 0; i < ((len > 16) ? 16 : len); i++) {
      fprintf(stderr, "%c", (buf[i] >= 32 && buf[i] < 128) ? buf[i] : '.');
    }
    fprintf(stderr, "\n");

    pos += 16;
    buf += 16;
    len -= 16;
  }
}

