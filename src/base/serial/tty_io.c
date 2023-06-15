/*
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
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <pwd.h>
#include <semaphore.h>
#include "emu.h"
#include "utilities.h"
#include "dosemu_config.h"
#include "ser_defs.h"
#include "tty_io.h"

/* This function flushes the internal unix receive buffer [num = port] */
static void tty_rx_buffer_dump(com_t *c)
{
  tcflush(c->fd, TCIFLUSH);
}

/* This function flushes the internal unix transmit buffer [num = port] */
static void tty_tx_buffer_dump(com_t *c)
{
  tcflush(c->fd, TCOFLUSH);
}

static int tty_get_tx_queued(com_t *c)
{
  int ret, queued;
  ret = ioctl(c->fd, TIOCOUTQ, &queued);
  if (ret < 0)
    return ret;
  return queued;
}

/* This function updates the line settings of a serial line (parity,
 * stop bits, word size, and baudrate) to conform to the value
 * stored in the Line Control Register (com[].LCR) and the Baudrate
 * Divisor Latch Registers (com[].dlm and com[].dll)     [num = port]
 */
static void tty_termios(com_t *c)
{
  speed_t baud;
  long int rounddiv;

  if (c->is_file)
    return;

  /* The following is the same as (com[num].dlm * 256) + com[num].dll */
  #define DIVISOR ((c->dlm << 8) | c->dll)

  s_printf("SER%d: LCR = 0x%x, ",c->num,c->LCR);

  /* Set the word size */
  c->newset.c_cflag &= ~CSIZE;
  switch (c->LCR & UART_LCR_WLEN8) {
  case UART_LCR_WLEN5:
    c->newset.c_cflag |= CS5;
    s_printf("5");
    break;
  case UART_LCR_WLEN6:
    c->newset.c_cflag |= CS6;
    s_printf("6");
    break;
  case UART_LCR_WLEN7:
    c->newset.c_cflag |= CS7;
    s_printf("7");
    break;
  case UART_LCR_WLEN8:
    c->newset.c_cflag |= CS8;
    s_printf("8");
    break;
  }

  /* Set the parity.  Rarely-used MARK and SPACE parities not supported yet */
  if (c->LCR & UART_LCR_PARITY) {
    c->newset.c_cflag |= PARENB;
    if (c->LCR & UART_LCR_EPAR) {
      c->newset.c_cflag &= ~PARODD;
      s_printf("E");
    }
    else {
      c->newset.c_cflag |= PARODD;
      s_printf("O");
    }
  }
  else {
    c->newset.c_cflag &= ~PARENB;
    s_printf("N");
  }

  /* Set the stop bits: UART_LCR_STOP set means 2 stop bits, 1 otherwise */
  if (c->LCR & UART_LCR_STOP) {
    /* This is actually 1.5 stop bit when word size is 5 bits */
    c->newset.c_cflag |= CSTOPB;
    s_printf("2, ");
  }
  else {
    c->newset.c_cflag &= ~CSTOPB;
    s_printf("1, ");
  }

  /* Linux 1.1.65 and above supports 115200 and 57600 directly, while
   * Linux 1.1.64 and below do not support them.  For these kernels, make
   * B115200 and B57600 equal to B38400.  These defines also may be
   * important if DOSEMU is ported to other nix-like operating systems.
   */
  #ifndef B115200
  #define B115200 B38400
  #endif
  #ifndef B57600
  #define B57600 B38400
  #endif

  /* The following sets the baudrate.  These nested IF statements rounds
   * upwards to the next higher baudrate. (ie, rounds downwards to the next
   * valid divisor value) The formula is:  bps = 1843200 / (divisor * 16)
   *
   * Note: 38400, 57600 and 115200 won't work properly if setserial has
   * the 'spd_hi' or the 'spd_vhi' setting turned on (they're obsolete!)
   */
  if ((DIVISOR < DIV_57600) && DIVISOR) {               /* 115200 bps */
    s_printf("bps = 115200, ");
    rounddiv = DIV_115200;
    baud = B115200;
  } else if (DIVISOR < DIV_38400) {                     /* 57600 bps */
    s_printf("bps = 57600, ");
    rounddiv = DIV_57600;
    baud = B57600;
  } else if (DIVISOR < DIV_19200) {			/* 38400 bps */
    s_printf("bps = 38400, ");
    rounddiv = DIV_38400;
    baud = B38400;
  } else if (DIVISOR < DIV_9600) {			/* 19200 bps */
    s_printf("bps = 19200, ");
    rounddiv = DIV_19200;
    baud = B19200;
  } else if (DIVISOR < DIV_4800) {			/* 9600 bps */
    s_printf("bps = 9600, ");
    rounddiv = DIV_9600;
    baud = B9600;
  } else if (DIVISOR < DIV_2400) {			/* 4800 bps */
    s_printf("bps = 4800, ");
    rounddiv = DIV_4800;
    baud = B4800;
  } else if (DIVISOR < DIV_1800) {			/* 2400 bps */
    s_printf("bps = 2400, ");
    rounddiv = DIV_2400;
    baud = B2400;
  } else if (DIVISOR < DIV_1200) {			/* 1800 bps */
    s_printf("bps = 1800, ");
    rounddiv = DIV_1800;
    baud = B1800;
  } else if (DIVISOR < DIV_600) {			/* 1200 bps */
    s_printf("bps = 1200, ");
    rounddiv = DIV_1200;
    baud = B1200;
  } else if (DIVISOR < DIV_300) {			/* 600 bps */
    s_printf("bps = 600, ");
    rounddiv = DIV_600;
    baud = B600;
  } else if (DIVISOR < DIV_150) {			/* 300 bps */
    s_printf("bps = 300, ");
    rounddiv = DIV_300;
    baud = B300;
  } else if (DIVISOR < DIV_110) {			/* 150 bps */
    s_printf("bps = 150, ");
    rounddiv = DIV_150;
    baud = B150;
  } else if (DIVISOR < DIV_50) {			/* 110 bps */
    s_printf("bps = 110, ");
    rounddiv = DIV_110;
    baud = B110;
  } else {						/* 50 bps */
    s_printf("bps = 50, ");
    rounddiv = DIV_50;
    baud = B50;
  }
  s_printf("divisor 0x%x -> 0x%lx\n", DIVISOR, rounddiv);

  /* The following does the actual system calls to set the line parameters */
  cfsetispeed(&c->newset, baud);
  cfsetospeed(&c->newset, baud);
  if (debug_level('s') > 7) {
    s_printf("SER%d: iflag=%x oflag=%x cflag=%x lflag=%x\n", c->num,
	    c->newset.c_iflag, c->newset.c_oflag,
	    c->newset.c_cflag, c->newset.c_lflag);
  }
}

static int tty_brkctl(com_t *c, int brkflg)
{
  int ret;
  /* there is change of break state */
  if (brkflg) {
    s_printf("SER%d: Setting BREAK state.\n", c->num);
    tcdrain(c->fd);
    ret = ioctl(c->fd, TIOCSBRK);
  } else {
    s_printf("SER%d: Clearing BREAK state.\n", c->num);
    ret = ioctl(c->fd, TIOCCBRK);
  }
  return ret;
}

static ssize_t tty_write(com_t *c, char *buf, size_t len)
{
  int fd;
  if (c->cfg->ro && c->wr_fd == -1)
    return len;
  fd = (c->wr_fd == -1 ? c->fd : c->wr_fd);
  return RPT_SYSCALL(write(fd, buf, len));   /* Attempt char xmit */
}

static int tty_dtr(com_t *c, int flag)
{
  int ret, control;
  control = TIOCM_DTR;
  if (flag)
    ret = ioctl(c->fd, TIOCMBIS, &control);
  else
    ret = ioctl(c->fd, TIOCMBIC, &control);
  return ret;
}

static int tty_rts(com_t *c, int flag)
{
  int ret, control;
  control = TIOCM_RTS;
  if (flag)
    ret = ioctl(c->fd, TIOCMBIS, &control);
  else
    ret = ioctl(c->fd, TIOCMBIC, &control);
  return ret;
}

/*  Determines if the tty is already locked.  Stolen from uri-dip-3.3.7k
 *  Nice work Uri Blumenthal & Ian Lance Taylor!
 *  [nam = complete path to lock file, return = nonzero if locked]
 */
static int tty_already_locked(char *nam)
{
  int  i = 0, pid = 0;
  FILE *fd = (FILE *)0;

  /* Does the lock file on our device exist? */
  if ((fd = fopen(nam, "re")) == (FILE *)0)
    return 0; /* No, return perm to continue */

  /* Yes, the lock is there.  Now let's make sure at least */
  /* there's no active process that owns that lock.        */
  if(config.tty_lockbinary)
    i = read(fileno(fd), &pid, sizeof(pid)) == sizeof(pid);
  else
    i = fscanf(fd, "%d", &pid);

  (void) fclose(fd);

  if (i != 1) /* Lock file format's wrong! Kill't */
    return 0;

  /* We got the pid, check if the process's alive */
  if (kill(pid, 0) == 0)      /* it found process */
    return 1;                 /* Yup, it's running... */

  /* Dead, we can proceed locking this device...  */
  return 0;
}

/*  Locks or unlocks a terminal line Stolen from uri-dip-3.3.7k
 *  Nice work Uri Blumenthal & Ian Lance Taylor!
 *  [path = device name,
 *   mode: 1 = lock, 2 = reaquire lock, anythingelse = unlock,
 *   return = zero if success, greater than zero for failure]
 */
static int tty_lock(const char *path, int mode)
{
  char saved_path[strlen(config.tty_lockdir) + 1 +
                  strlen(config.tty_lockfile) +
                  strlen(path) + 1];
  struct passwd *pw;
  pid_t ime;
  const char *slash;

  if (path == NULL) return(0);        /* standard input */
  slash = strrchr(path, '/');
  if (slash == NULL)
    slash = path;
  else
    slash++;

  sprintf(saved_path, "%s/%s%s", config.tty_lockdir, config.tty_lockfile,
	  slash);

  if (mode == 1) {      /* lock */
    {
      FILE *fd;
      if (tty_already_locked(saved_path) == 1) {
        error("attempt to use already locked tty %s\n", saved_path);
        return (-1);
      }
      unlink(saved_path);	/* kill stale lockfiles, if any */
      fd = fopen(saved_path, "we");
      if (fd == (FILE *)0) {
        error("tty: lock: (%s): %s\n", saved_path, strerror(errno));
        return(-1);
      }

      ime = getpid();
      if(config.tty_lockbinary)
	write (fileno(fd), &ime, sizeof(ime));
      else
	fprintf(fd, "%10d\n", (int)ime);

      (void)fclose(fd);
    }

    /* Make sure UUCP owns the lockfile.  Required by some packages. */
    if ((pw = getpwnam(owner_tty_locks)) == NULL) {
      error("tty: lock: UUCP user %s unknown!\n", owner_tty_locks);
      return(0);        /* keep the lock anyway */
    }

    (void) chown(saved_path, pw->pw_uid, pw->pw_gid);
    (void) chmod(saved_path, 0644);
  }
  else if (mode == 2) { /* re-acquire a lock after a fork() */
    FILE *fd;

     fd = fopen(saved_path,"we");
     if (fd == (FILE *)0) {
      error("tty_lock: reacquire (%s): %s\n",
              saved_path, strerror(errno));
      return(-1);
    }
    ime = getpid();

    if(config.tty_lockbinary)
      write (fileno(fd), &ime, sizeof(ime));
    else
      fprintf(fd, "%10d\n", (int)ime);

    (void) fclose(fd);
    (void) chmod(saved_path, 0440);
    return(0);
  }
  else {    /* unlock */
    FILE *fd;
    int retval;

    fd = fopen(saved_path,"we");
    if (fd == (FILE *)0) {
      error("tty_lock: can't reopen %s to delete: %s\n",
             saved_path, strerror(errno));
      return (-1);
    }

    retval = unlink(saved_path);
    if (retval < 0) {
      error("tty: unlock: (%s): %s\n", saved_path,
             strerror(errno));
      fclose(fd);
      return -1;
    }
  }
  return(0);
}

static void ser_set_params(com_t *c)
{
  int data = 0;

  /* Return if not a tty */
  if (tcgetattr(c->fd, &c->newset) == -1) {
    if(s1_printf) s_printf("SER%d: Line Control: NOT A TTY (%s).\n",c->num,strerror(errno));
    return;
  }
  c->newset.c_cflag = CS8 | CLOCAL | CREAD;
  c->newset.c_iflag = IGNBRK | IGNPAR;
  c->newset.c_oflag = 0;
  c->newset.c_lflag = 0;
  if (c->cfg->virt) {
    c->newset.c_lflag |= ISIG;
    c->newset.c_cc[VERASE] = 8;  // set to BS, its DEL by default
  }

#ifdef __linux__
  c->newset.c_line = 0;
#endif
  c->newset.c_cc[VMIN] = 1;
  c->newset.c_cc[VTIME] = 0;
  if (c->cfg->system_rtscts)
    c->newset.c_cflag |= CRTSCTS;

  if(s2_printf) s_printf("SER%d: do_ser_init: running ser_termios\n", c->num);
  tty_termios(c);			/* Set line settings now */
  tcsetattr(c->fd, TCSANOW, &c->newset);

  /* Pull down DTR and RTS.  This is the most natural for most comm */
  /* devices including mice so that DTR rises during mouse init.    */
  if (!c->cfg->pseudo) {
    data = TIOCM_DTR | TIOCM_RTS;
    if (ioctl(c->fd, TIOCMBIC, &data) && errno == EINVAL) {
      s_printf("SER%d: TIOCMBIC unsupported, setting pseudo flag\n", c->num);
      c->cfg->pseudo = 1;
    }
  }
}

/* This function checks for newly received data and fills the UART
 * FIFO (16550 mode) or receive register (16450 mode).
 *
 * Note: The receive buffer is now a sliding buffer instead of
 * a queue.  This has been found to be more efficient here.
 *
 * [num = port]
 */
static int tty_uart_fill(com_t *c)
{
  int size = 0;

  if (c->fd < 0)
    return 0;

  /* Return if in loopback mode */
  if (c->MCR & UART_MCR_LOOP)
    return 0;

  /* Is it time to do another read() of the serial device yet?
   * The rx_timer is used to prevent system load caused by empty read()'s
   * It also skip the following code block if the receive buffer
   * contains enough data for a full FIFO (at least 16 bytes).
   * The receive buffer is a sliding buffer.
   */
  if (RX_BUF_BYTES(c->num) >= RX_BUFFER_SIZE) {
    if(s3_printf) s_printf("SER%d: Too many bytes (%i) in buffer\n", c->num,
        RX_BUF_BYTES(c->num));
    return 0;
  }

  /* Slide the buffer contents to the bottom */
  rx_buffer_slide(c->num);

  /* Do a block read of data.
   * Guaranteed minimum requested read size of (RX_BUFFER_SIZE - 16)!
   */
  size = RPT_SYSCALL(read(c->fd,
                              &c->rx_buf[c->rx_buf_end],
                              RX_BUFFER_SIZE - c->rx_buf_end));
  ioselect_complete(c->fd);
  if (size <= 0)
    return 0;
  if(s3_printf) s_printf("SER%d: Got %i bytes, %i in buffer\n", c->num,
        size, RX_BUF_BYTES(c->num));
  if (debug_level('s') >= 9) {
    int i;
    for (i = 0; i < size; i++)
      s_printf("SER%d: Got data byte: %#x\n", c->num,
          c->rx_buf[c->rx_buf_end + i]);
  }
  c->rx_buf_end += size;
  return size;
}

static void async_serial_run(int fd, void *arg)
{
  com_t *c = arg;
  int size;
  s_printf("SER%d: Async notification received\n", c->num);
  size = tty_uart_fill(c);
  if (size > 0)
    receive_engine(c->num, size);
}

static int ser_open_existing(com_t *c)
{
  struct stat st;
  int err, io_sel = 0, oflags = O_NONBLOCK;

  err = stat(c->cfg->dev, &st);
  if (err) {
    error("SERIAL: stat(%s) failed: %s\n", c->cfg->dev,
	    strerror(errno));
    c->fd = -2;
    return -1;
  }
  if (S_ISFIFO(st.st_mode)) {
    s_printf("SER%i: %s is fifo, setting pseudo flag\n", c->num,
	    c->cfg->dev);
    c->is_file = TRUE;
    c->cfg->pseudo = TRUE;
    /* force read-only to avoid SIGPIPE */
    c->cfg->ro = TRUE;
    oflags |= O_RDONLY;
    io_sel = 1;
  } else {
    if (S_ISREG(st.st_mode)) {
      s_printf("SER%i: %s is file, setting pseudo flag\n", c->num,
	    c->cfg->dev);
      c->is_file = TRUE;
      c->cfg->pseudo = TRUE;
      oflags |= O_RDONLY;
      if (!c->cfg->ro && !c->cfg->wrfile)
        c->wr_fd = RPT_SYSCALL(open(c->cfg->dev, O_WRONLY | O_APPEND));
    } else {
      oflags |= O_RDWR;
      io_sel = 1;
    }
  }

  c->fd = RPT_SYSCALL(open(c->cfg->dev, oflags));
  if (c->fd < 0) {
    error("SERIAL: Unable to open device %s: %s\n",
      c->cfg->dev, strerror(errno));
    return -1;
  }

  if (!c->is_file && !isatty(c->fd)) {
    s_printf("SERIAL: Serial port device %s is not a tty\n",
      c->cfg->dev);
    c->is_file = TRUE;
    c->cfg->pseudo = TRUE;
  }

  if (!c->is_file) {
    RPT_SYSCALL(tcgetattr(c->fd, &c->oldset));
    RPT_SYSCALL(tcgetattr(c->fd, &c->newset));
#if 0
    if (c->cfg->low_latency) {
      struct serial_struct ser_info;
      int err = ioctl(c->fd, TIOCGSERIAL, &ser_info);
      if (err) {
        error("SER%d: failure getting serial port settings, %s\n",
          c->num, strerror(errno));
      } else {
        ser_info.flags |= ASYNC_LOW_LATENCY;
        err = ioctl(c->fd, TIOCSSERIAL, &ser_info);
        if (err)
          error("SER%d: failure setting low_latency flag, %s\n",
            c->num, strerror(errno));
        else
          s_printf("SER%d: low_latency flag set\n", c->num);
      }
    }
#endif
    ser_set_params(c);
  }
  if (io_sel)
    add_to_io_select(c->fd, async_serial_run, (void *)c);
  return 0;
}

static int pty_init(com_t *c)
{
    int pty_fd = posix_openpt(O_RDWR);
    if (pty_fd == -1)
    {
        error("openpt failed %s\n", strerror(errno));
        return -1;
    }
    unlockpt(pty_fd);
    fcntl(pty_fd, F_SETFL, O_NONBLOCK);
    return pty_fd;
}

static int tty_close(com_t *c);

static void pty_exit(void *arg)
{
  com_t *c = arg;
  error("pty process terminated\n");
  tty_close(c);
}

static int pty_open(com_t *c, const char *cmd)
{
  char sem_name[256];
  sem_t *pty_sem;
  struct termios t;
  const char *argv[] = { "sh", "-c", cmd, NULL };
  const int argc = 4;
  int pty_fd;

  snprintf(sem_name, sizeof(sem_name), "/dosemu_serpty_sem_%i", getpid());
  pty_sem = sem_open(sem_name, O_CREAT, S_IRUSR | S_IWUSR, 0);
  if (!pty_sem)
  {
    error("sem_open failed %s\n", strerror(errno));
    return -1;
  }
  sem_unlink(sem_name);
  pty_fd = pty_init(c);
  pid_t pid = run_external_command("/bin/sh", argc, argv,
      1, -1, pty_fd, pty_sem);
  if (pid == -1)
    return -1;
  /* wait for slave to open pts */
  sem_wait(pty_sem);
  sem_close(pty_sem);
  sigchld_register_handler(pid, pty_exit, c);
  c->pty_pid = pid;
  cfmakeraw(&t);
  tcsetattr(pty_fd, TCSANOW, &t);
  return pty_fd;
}

static int pty_close(com_t *c, int fd)
{
  sigchld_enable_handler(c->pty_pid, 0);
  return close(fd);
}

/* This function opens ONE serial port for DOSEMU.  Normally called only
 * by do_ser_init below.   [num = port, return = file descriptor]
 */
static int tty_open(com_t *c)
{
  int err;

  if (c->cfg->exec) {
    c->fd = pty_open(c, c->cfg->exec);
    if (c->fd == -1)
      return -1;
    c->cfg->pseudo = TRUE;
    add_to_io_select(c->fd, async_serial_run, (void *)c);
    return c->fd;
  }
  if (c->fd != -1)
    return -1;
  s_printf("SER%d: Running ser_open, %s, fd=%d\n", c->num,
	c->cfg->dev, c->fd);

  if (c->fd != -1)
    return (c->fd);

  if (c->cfg->virt)
  {
    /* don't try to remove any lock: they don't make sense for ttyname(0) */
    s_printf("Opening Virtual Port\n");
    c->dev_locked = FALSE;
  } else if (config.tty_lockdir[0]) {
    if (tty_lock(c->cfg->dev, 1) >= 0) {		/* Lock port */
      /* We know that we have access to the serial port */
      /* If the port is used for a mouse, then don't lock, because
       * the use of the mouse serial port can be switched between processes,
       * such as on Linux virtual consoles.
       */
      c->dev_locked = TRUE;
    } else {
      /* The port is in use by another process!  Don't touch the port! */
      c->dev_locked = FALSE;
      c->fd = -2;
      return(-1);
    }
  } else {
    s_printf("Warning: Port locking disabled in the config.\n");
    c->dev_locked = FALSE;
  }

  err = access(c->cfg->dev, F_OK);
  if (!err) {
    err = ser_open_existing(c);
    if (err)
      goto fail_unlock;
  } else {
    c->fd = open(c->cfg->dev, O_WRONLY | O_CREAT | O_EXCL, 0640);
    if (c->fd == -1) {
      error("SER%i: unable to open or create %s\n", c->num, c->cfg->dev);
      goto fail_unlock;
    }
  }
  if (c->cfg->wrfile) {
    c->wr_fd = open(c->cfg->wrfile, O_WRONLY | O_CREAT | O_TRUNC, 0640);
    if (c->wr_fd == -1) {
      error("SER%i: unable to open or create for write %s\n", c->num, c->cfg->dev);
      goto fail_unlock;
    }
  }

  modstat_engine(c->num);
  return c->fd;

  close(c->fd);
  /* fall through */
fail_unlock:
  if (c->dev_locked && tty_lock(c->cfg->dev, 0) >= 0) /* Unlock port */
    c->dev_locked = FALSE;

  c->fd = -2; // disable permanently
  return -1;
}

/* This function closes ONE serial port for DOSEMU.  Normally called
 * only by do_ser_init below.   [num = port, return = file error code]
 */
static int tty_close(com_t *c)
{
  int ret;
  if (c->fd < 0)
    return -1;
  if (c->wr_fd != -1) {
    close(c->wr_fd);
    c->wr_fd = -1;
  }
  s_printf("SER%d: Running ser_close\n", c->num);
  remove_from_io_select(c->fd);
  if (c->cfg->exec) {
    ret = pty_close(c, c->fd);
    c->fd = -1;
    return ret;
  }

  /* save current dosemu settings of the file and restore the old settings
   * before closing the file down.
   */
  if (!c->is_file) {
    RPT_SYSCALL(tcgetattr(c->fd, &c->newset));
    RPT_SYSCALL(tcsetattr(c->fd, TCSANOW, &c->oldset));
  }
  ret = RPT_SYSCALL(close(c->fd));
  c->fd = -1;

  /* Clear the lockfile from DOSEMU */
  if (c->dev_locked) {
    if (tty_lock(c->cfg->dev, 0) >= 0)
      c->dev_locked = FALSE;
  }
  return ret;
}

static int tty_get_msr(com_t *c)
{
  int control, err;
  err = ioctl(c->fd, TIOCMGET, &control);
  if (err)
    return 0;
  return (((control & TIOCM_CTS) ? UART_MSR_CTS : 0) |
          ((control & TIOCM_DSR) ? UART_MSR_DSR : 0) |
          ((control & TIOCM_RNG) ? UART_MSR_RI : 0) |
          ((control & TIOCM_CAR) ? UART_MSR_DCD : 0));
}


struct serial_drv tty_drv = {
  tty_rx_buffer_dump,
  tty_tx_buffer_dump,
  tty_get_tx_queued,
  tty_termios,
  tty_brkctl,
  tty_write,
  tty_dtr,
  tty_rts,
  tty_open,
  tty_close,
  tty_uart_fill,
  tty_get_msr,
  "tty_io"
};
