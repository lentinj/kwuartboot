/*
 * Copyright (C) 2011
 * Leigh Brown <leigh@solinno.co.uk>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define SOH	0x01
#define STX	0x02
#define EOT	0x04
#define ACK	0x06
#define NAK	0x15
#define CAN	0x18
#define CTRLZ	0x1A

#define PATTERN_SEND_INTERVAL	(50000)		/* 50 milliseconds */
#define PATTERN_SEND_TIMEOUT	20		/* seconds */
#define ONE_SECOND_US		(1000000)	/* 1 second */

#define RECEIVE_TIMEOUT		(60 * 1000000)	/* 60 seconds */

#define MAX_RETRANS		(10)

const unsigned char boot_pattern[] =
	{ 0xbb, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77 };

char *argv0;

void
usage(void)
{
    fprintf(stderr, 
	    "Usage: %s <serial device> <filename>\n", argv0);
    exit(1);
}

int
read_block(int fd, unsigned char *buf, int size)
{
    int tot = 0;
    int rem = size;
    int got;
    
    while (rem > 0) {
	got = read(fd, buf, rem);
	if (got == 0)
	    break;	    
	if (got < 0)
	    return -1; /* errno will be set to the error */

	tot += got;
	buf += got;
	rem -= got;
    }

    /* Fill the remainder of the packet with zeroes */
    if (rem > 0)
	memset(buf, 0, rem);

    return tot;
}

int
read_byte(int fd, unsigned int timeout)
{
    fd_set rfds;
    struct timeval tv;
    unsigned char byte;
    int got;
    int retval;

    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    tv.tv_sec  = timeout / ONE_SECOND_US;
    tv.tv_usec = timeout % ONE_SECOND_US;
    
    retval = select(fd + 1, &rfds, NULL, NULL, &tv);
    if (retval == 0)
	errno = ETIMEDOUT; /* synthesize our own error code for timeout */
    if (retval < 1)
	return -1;

    got = read(fd, &byte, 1);
    assert(got != 0);
    if (got < 0)
	return -1; /* errno will be set to the error */

    return (int)byte;
}

static inline int
write_byte(int fd, int c)
{
    unsigned char byte;

    byte = (unsigned char)c;
    return write(fd, &byte, 1);
}

void
rotator(void)
{
    static const char rotator[] = { '|', '/', '-', '\\' };
    static int pos = 0;
    printf("%c\b", rotator[pos]);
    fflush(stdout);
    pos = (pos + 1) % 4;
}

void
cancel_send(int fd)
{
    static const unsigned char cancel[] = { CAN, CAN, CAN };

    if (write(fd, cancel, sizeof(cancel)) == sizeof(cancel))
    	if (!tcdrain(fd))
	    tcflush(fd, TCIOFLUSH);
}

int
send_boot_pattern(int fd, int timeout)
{
    int wrote;
    int c;
    int done;
    time_t start, now;

    /* Make a note of the time we start */
    start = time(NULL);

    /* Flush any pending input or output */
    tcflush(fd, TCIOFLUSH);

    /* Keep sending the boot pattern until we timeout or we get x-modem NAK */
    printf("Sending boot pattern: power on or reset now...");
    
    done = 0;
    while (!done) {
	rotator();
	wrote = write(fd, boot_pattern, sizeof(boot_pattern));
	if (wrote == -1)
	    return -1;

	assert(wrote == sizeof(boot_pattern)); /* assumption */

	/* Wait for the queue to drain before checking for a response */
	if (tcdrain(fd) < 0)
	    return -1;

	/* read any returned characters */
	for (;;) {
	    c = read_byte(fd, PATTERN_SEND_INTERVAL);
	    if (c == -1) {
		if (errno == ETIMEDOUT)
		    break;
		return -1;
	    }

	    if (c == NAK) { /* System is waiting for x-modem packet */
		done = 1;
		break;
	    }

	    /* Ignore any echoed characters from the boot pattern itself */
	    if (memchr(boot_pattern, c, sizeof(boot_pattern)) == NULL) {
		printf("*.");
		fflush(stdout);
	    }
	}

	/* Check for timeout */
	now = time(NULL);
	if (now - start > timeout) {
	    printf("timeout\n");
	    return 1;
	}
    }

    printf("done\n");
    return 0;
}

int
wait_for_nak(int fd)
{
    int c;

    c = read_byte(fd, RECEIVE_TIMEOUT);
    if (c == -1) {
	fprintf(stderr, "%s: error reading from serial port: %m\n", argv0);
	return -1;
    }

    assert(c >= 0 && c <= 255);

    switch (c) {
	case 'C':
	    printf("unexpected CRC-mode character\n");
	    return -1;
	case NAK:
	    break;
	case CAN: /* request by remote to cancel transmission */
	    /* wait for a second CAN, just in case */
	    if ((c = read_byte(fd, ONE_SECOND_US)) == CAN) {
		write_byte(fd, ACK);
		printf("cancelled by remote\n");
		return -1;
	    }
	    break;
	default:
	    printf("unexpected character %02x, cancelling send\n", c);
	    cancel_send(fd);
	    return -1;
    }

    return 0;
}

int
build_packet(int fd, int packetno, unsigned char packet[], int buflen)
{
    int got, i;

    assert(buflen > 0);

    /* Fill in the header */
    packet[0] = SOH;
    packet[1] =  (unsigned char)packetno;
    packet[2] = ~(unsigned char)packetno;

    /* Read the next block from the file */
    got = read_block(fd, &packet[3], buflen);
    if (got == -1) {
	fprintf(stderr, "%s: error reading from file: %m\n", argv0);
	return -1;
    }
    if (got == 0)
	return 1;

    /* Calculate checksum and insert at the end of the packet */
    unsigned char chksum = 0;
    for (i = 3; i < buflen + 3; ++i) {
	    chksum += packet[i];
    }
    packet[buflen + 3] = chksum;

    return 0;
}

int
send_packet(int fd, unsigned char packet[], int len)
{
    int i;

    /* Send the complete packet across the serial line */
    for (i = 0; i < len; ++i) {
	if (write_byte(fd, packet[i]) == -1)
	    return -1;

	/* Flow control doesn't seem to work very well so we
	   use tcdrain to ensure we don't send too fast */
	if (i % 8 == 0)
	    if (tcdrain(fd) < 0)
		return -1;
    }

    /* Return the response */
    return read_byte(fd, RECEIVE_TIMEOUT);
}

int
xmodem_send(int tty_fd, int in_fd)
{
    unsigned char packet[134];
    static const int buflen = 128;
    int packetno, lastpacket;
    int c;
    int retry;
    int res;

    printf("Sending file...");

    /* We have already received a NAK, but wait for another to be sure */
    if (wait_for_nak(tty_fd) == -1)
	return -1;

    lastpacket = 0;
    packetno   = 1;
    for (retry = 0; retry < MAX_RETRANS; ++retry) {
	rotator();

	if (packetno != lastpacket) {
	    res = build_packet(in_fd, packetno, packet, buflen);
	    if (res == -1) return -1;
	    if (res ==  1) break;
	    lastpacket = packetno;
	}
	else
	    printf("*%d*.", retry);

	c = send_packet(tty_fd, packet, buflen + 4);
	if (c == -1) {
	    if (errno == ETIMEDOUT)
		continue; /* retry if timed out */

	    fprintf(stderr, 
		    "%s: error writing to serial port: %m\n", argv0);
	    break;
	}

	switch (c) {
	    case ACK:
		retry = 0;
		++packetno;
		break;
	    case CAN:
		printf("cancelled by remote\n");
		if ((c = read_byte(tty_fd, 1)) == CAN) {
		    write_byte(tty_fd, ACK);
		    tcdrain(tty_fd);
		    return -1;
		}
		break;
	    default:
		printf("unexpected character %02x\n", c);
	    case NAK:
		break;
	}
    }
    if (retry == MAX_RETRANS) {
	fprintf(stderr, "Too many retries, cancelling send\n");
	cancel_send(tty_fd);
	return -1; /* xmit error */
    }

    printf("\nFinishing...");
    for (retry = 0; retry < MAX_RETRANS; ++retry) {
	    write_byte(tty_fd, EOT);
	    tcdrain(tty_fd);
	    if ((c = read_byte(tty_fd, 1)) == ACK)
		break;
    }

    if (c != ACK) {
	puts("failed\n");
	return -1;
    }
    puts("done\n");
    return 0;
}

int
main(int argc, char *argv[])
{
    struct termios tio;
    char *dev;
    char *fname;
    int tty_fd, file_fd;
    int res;

    argv0 = argv[0];

    if (argc != 3)
	usage();

    dev   = argv[1];
    fname = argv[2];

    memset(&tio, 0, sizeof(tio));
    tio.c_iflag = 0;
    tio.c_oflag = 0;
    tio.c_cflag = CS8|CREAD|CLOCAL;
    tio.c_lflag = 0;
    tio.c_cc[VMIN]  = 1;
    tio.c_cc[VTIME] = 5;

    tty_fd = open(dev, O_RDWR | O_NONBLOCK);
    if (tty_fd < 0) {
	fprintf(stderr, "%s: unable to open '%s': %m\n", argv0, dev);
	exit(1);
    }

    cfsetospeed(&tio, B115200);
    cfsetispeed(&tio, B115200);
    tcsetattr(tty_fd, TCSANOW, &tio);

    file_fd = open(fname, O_RDONLY);
    if (file_fd < 0) {
	fprintf(stderr, "%s: unable to open '%s': %m\n", argv0, dev);
	exit(1);
    }

    res = send_boot_pattern(tty_fd, PATTERN_SEND_TIMEOUT);
    switch (res) {
	case 0:
	    xmodem_send(tty_fd, file_fd);
	    break;
	case -1:
	    fprintf(stderr, "%s: failed to send boot pattern: %m\n", argv0);
	case 1:
	    break;
    }
    
    close(tty_fd);
    close(file_fd);

    return 0;
}
