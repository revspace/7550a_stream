/** \file 7550a_stream.c
 * \brief Utility to implement manual software flow control for the HP 7550A
 * plotter.
 *
 * For more information about the general tactic and device commands
 * used, see the HP 7550A Interfacing and Programming Manual (available
 * from the HP Computer Museum website at
 * http://www.hpmuseum.net/exhibit.php?hwdoc=75.
 *
 * \todo There is some weird issue where the plotter does not seem to
 * be able to parse commands in the first two-or-so chunks of data.
 *
 * \todo There is probably some defensive programming and error handling
 * missing here that would be nice to have.
 *
 * \todo Maybe implement a better indication of progress to be more
 * user-friendly?
 */

#include <stddef.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>

#define QUERY_DELAY	50000	/**< Delay in us between buffer-capacity queries */
#define READBUFSIZE	64	/**< Serial read buffer size */

#define DEVCOM_PREFIX	"\x1B."	/**< Device command prefix */
#define DEVCOM_TERM	'\r'	/**< Device command terminator */
#define DEVCOM_SEP	','	/**< Device command separator */

/** Initialises the serial port. */
int init_serial(const char *device, int baud_rate) {
	int fd;
	struct termios options;

	/* open the serial port, handle errors */
	fd = open(device, O_RDWR | O_NOCTTY);

	if(fd < 0) {
		fprintf(stderr, "could not open serial port: %s\n", strerror(errno));
		return fd;
	}

	/* configure serial port */
	tcgetattr(fd, &options);

	cfsetispeed(&options, baud_rate);
	cfsetospeed(&options, baud_rate);

	options.c_cflag &= ~CSIZE;					/* 8-bits characters */
	options.c_cflag |=  CS8;

	options.c_cflag |=  PARENB | PARODD;				/* odd parity */
	options.c_cflag &= ~CSTOPB;

	options.c_cflag &= ~(CRTSCTS);					/* disable HW flow control */
	options.c_cflag |=  (CLOCAL | CREAD);				/* local & enable receiver */
	options.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);		/* raw mode */
	options.c_iflag |=  (INPCK | ISTRIP);				/* check & strip parity */
	options.c_iflag &= ~(IXON | IXOFF | IXANY);			/* disable software flow control */
	options.c_oflag &= ~OPOST;					/* raw output */

	/* set configuration, handle errors */
	if(tcsetattr(fd, TCSAFLUSH, &options) < 0) {
		fprintf(stderr, "could not set serial port attributes: %s\n", strerror(errno));
		close(fd);
		return -1;
	}

	return fd;
}

/** Write a string to the serial port. */
void serial_write(int fd, const char *s) {
	int len;

	len = strlen(s);
	if(write(fd, s, len) != len) {
		fprintf(stderr, "partial write to port: %s\n", strerror(errno));
	}
}

/** Read in what the plotter manual calls a DEC field from the plotter.
 * It consists of a number, encoded as the digits in ASCII, followed by
 * the device command terminator character.
 */
int read_dec(int fd) {
	char	read_buffer[READBUFSIZE];
	char	dec_buffer[READBUFSIZE];
	int	rbuf_len;
	int	rbuf_pos, dbuf_pos;
	int	run;

	run = 1;
	dbuf_pos = 0;
	while(1) {
		rbuf_len = read(fd, read_buffer, 1);
		for(rbuf_pos = 0; rbuf_pos < rbuf_len; rbuf_pos++) {
			if(read_buffer[rbuf_pos] == DEVCOM_TERM) {
				dec_buffer[dbuf_pos] = '\0';	/* terminate buffer */
				return atoi(dec_buffer);
			} else {
				dec_buffer[dbuf_pos++] = read_buffer[rbuf_pos];
			}
		}
	}

}

/** Read in what the plotter manual calls ASC field from the plotter.
 * It consists of a string in ASCII, followed by the device command
 * terminator character.
 */
char *read_asc(int fd) {
	char	read_buffer[READBUFSIZE];
	char	asc_buffer[READBUFSIZE];
	int	rbuf_len;
	int	rbuf_pos, dbuf_pos;
	int	run;

	run = 1;
	dbuf_pos = 0;
	while(1) {
		rbuf_len = read(fd, read_buffer, 1);
		for(rbuf_pos = 0; rbuf_pos < rbuf_len; rbuf_pos++) {
			if(read_buffer[rbuf_pos] == DEVCOM_TERM) {
				asc_buffer[dbuf_pos] = '\0';	/* terminate buffer */
				return strdup(asc_buffer);
			} else {
				asc_buffer[dbuf_pos++] = read_buffer[rbuf_pos];
			}
		}
	}

}

/** Program entrypoint */
int main(int argc, char **argv) {
	/* options */
	char	*device;
	char	*filename;
	int	 verbose;
	int	 baud_rate;

	/* internal vars */
	char c;

	char *ident;
	size_t bufsize, chunksize;
	size_t buffree, fbufsize;
	char *buffer;
	int serial_fd;
	FILE *input;

	/* default options */
	device		= "/dev/ttyS0";
	filename	= NULL;
	verbose		= 0;
	baud_rate	= B9600;

	opterr = 0;

	/* Process command-line arguments */
	while ((c = getopt (argc, argv, "d:f:r:v")) != -1) {
		switch (c) {
			case 'd':
				device = optarg;
				break;

			case 'f':
				filename = optarg;
				break;

			case 'r':
				fprintf(stderr, "warning: baud-rate setting not implemented. kick lazy developer.\n");
				break;

			case 'v':
				verbose = 1;
				break;

			case '?':
				if (optopt == 'd' || optopt == 'f' || optopt == 'r')
					fprintf(stderr, "error: option -%c requires an argument.\n", optopt);
				else if (isprint (optopt))
					fprintf (stderr, "error: unknown option '-%c'.\n", optopt);
				else
					fprintf (stderr, "error: unknown option '\\x%x'.\n", optopt);

				return EXIT_FAILURE;

			default:
				abort();
		}
	}

	/* open file */
	if(filename == NULL) {
		if(verbose) fprintf(stderr, "using stdin as input\n");
		input = stdin;
	} else {
		if(verbose) fprintf(stderr, "opening file %s\n", filename);
		input = fopen(filename, "r");

		if(input == NULL) {
			fprintf(stderr, "error opening input file: %s\n", strerror(errno));
			return EXIT_FAILURE;
		}
	}


	/* open port */
	if(verbose) fprintf(stderr, "initialising serial port %s\n", device);

	serial_fd = init_serial(device, baud_rate);
	if(serial_fd < 0) {
		if(filename != NULL) fclose(input);
		return EXIT_FAILURE;
	}

	/* init plotter */
	if(verbose) fprintf(stderr, "sending plotter init sequence...\n");
	serial_write(serial_fd, DEVCOM_PREFIX "(");			/* plotter on */
	serial_write(serial_fd, DEVCOM_PREFIX "R");			/* reset */
	serial_write(serial_fd, DEVCOM_PREFIX "L");			/* wait for reset to complete, output buffer size */
	bufsize = read_dec(serial_fd);

	chunksize = bufsize / 2;
	if(verbose) fprintf(stderr, "buffer size: %u (chunk size %u)\n",
				(unsigned int) bufsize,
				(unsigned int) chunksize);

	if(verbose) {
		serial_write(serial_fd, DEVCOM_PREFIX "A");
		ident = read_asc(serial_fd);
		fprintf(stderr, "plotter identification string: %s\n", ident);
	}

	serial_write(serial_fd, DEVCOM_PREFIX "L");
	serial_write(serial_fd, DEVCOM_PREFIX "L");

	/* send file in chunks */
	serial_write(serial_fd, DEVCOM_PREFIX "U");

	if(verbose) fprintf(stderr, "starting to send file...\n");

	buffer = malloc(chunksize);

	/* Spool the input file to the plotter. This is done by reading
	 * in a chunk, then polling the plotter until it has the
	 * required space available in its buffer so we can send another
	 * chunk, etc.
	 */
	while(!feof(input)) {
		fbufsize = fread(buffer, 1, chunksize, input);

		buffree = 0;
		while(buffree <= fbufsize) {
			usleep(QUERY_DELAY);				/* let the plotter process some data */
			serial_write(serial_fd, DEVCOM_PREFIX "B");	/* query buffer size */
			buffree = read_dec(serial_fd);			/* read in buffer size */

			if(verbose) fprintf(stderr, "%u free\n", (unsigned int) buffree);
		}

		/* we have available space! */
		if(verbose) fprintf(stderr, "writing %u-sized chunk to serial port:\n", (unsigned int) fbufsize);

		write(serial_fd, buffer, fbufsize);
	}

	/* shutdown plotter */
	if(verbose) fprintf(stderr, "sending plotter shutdown sequence...\n");

	serial_write(serial_fd, DEVCOM_PREFIX ")");			/* plotter off */

	/* cleanup */
	free(ident);
	close(serial_fd);
	fclose(input);

	if(verbose) fprintf(stderr, "all done.\n");

	return EXIT_SUCCESS;
}
