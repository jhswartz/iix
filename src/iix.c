#define _XOPEN_SOURCE 600

#include <sys/types.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <signal.h>
#include <termios.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <errno.h>

struct iix_input
{
	int descriptor;

	struct iix_input *previous;
	struct iix_input *next;
};

struct iix_inputs
{
	fd_set descriptors;
	int highest_descriptor;

	struct iix_input *head;
	struct iix_input *tail;
};

struct iix_pty
{
	int master;
	int slave;
};

struct iix_ctty
{
	struct termios attributes;
};

struct iix_program
{
	char *name;
	char **arguments;
};

static struct iix_ctty ctty;
static struct iix_pty pty;
static struct iix_inputs inputs;
static struct iix_program program;

static bool multiplexing = false;

static bool parse_command_line(int argc, char *argv[]);
static void display_usage(FILE *stream);
static void cleanup(void);

static bool handle_signals(void);

static bool reconfigure_ctty(void);
static void reset_ctty(void);

static bool open_pty(void);
static int open_pty_master(void);
static int open_pty_slave(int master);

static bool reconfigure_stdio(void);

static void track_descriptor(int descriptor);
static void untrack_descriptor(int descriptor);
static void find_highest_descriptor(void);

static bool enable_blocking(int descriptor, bool enable);

static bool add_standard_input(void);
static bool add_file_input(const char *filename);
static bool add_pipe_input(const char *filename);
static bool add_input(int descriptor);

static bool allocate_input(int descriptor);
static void remove_inputs(void);

static void link_input(struct iix_input *input);
static void unlink_input(struct iix_input *input);

static bool execute_program(void);

static bool multiplex(void);
static void stop_multiplexing();

static bool service(struct iix_input *input, int output);

int main(int argc, char *argv[])
{
	if (!isatty(STDIN_FILENO))
	{
		return EXIT_FAILURE;
	}

	if (atexit(cleanup))
	{
		return EXIT_FAILURE;
	}

	if (!parse_command_line(argc, argv))
	{
		return EXIT_FAILURE;
	}

	if (!handle_signals())
	{
		return EXIT_FAILURE;
	}

	if (!reconfigure_ctty())
	{
		return EXIT_FAILURE;
	}

	if (!open_pty())
	{
		return EXIT_FAILURE;
	}

	pid_t pid = fork();

	if (pid == -1)
	{
		return EXIT_FAILURE;
	}

	else if (pid == 0)
	{
		close(pty.master);

		if (!execute_program())
		{
			return EXIT_FAILURE;
		}
	}

	else
	{
		close(pty.slave);

		if (!multiplex())
		{
			return EXIT_FAILURE;
		}
	}

	return EXIT_SUCCESS;
}

static bool parse_command_line(int argc, char *argv[])
{
	while (true)
	{
		int option = getopt(argc, argv, "hf:p:t:");

		if (option == -1)
		{
			break;
		}

		else if (option == 'h')
		{
			display_usage(stdout);
			exit(EXIT_SUCCESS);
		}

		else if (option == 'f')
		{
			if (!add_file_input(optarg))
			{
				return false;
			}
		}

		else if (option == 'p')
		{
			if (!add_pipe_input(optarg))
			{
				return false;
			}
		}

		else
		{
			display_usage(stderr);
			return false;
		}
	}

	if (optind >= argc)
	{
		display_usage(stderr);
		return false;
	}

	program.name = *(argv + optind);
	program.arguments = argv + optind;

	return true;
}

static void cleanup(void)
{
       	remove_inputs();
}

static void display_usage(FILE *stream)
{
	fprintf(stream, "\n");
	fprintf(stream, "Usage:\n");
	fprintf(stream, " iix [options] program [arguments]\n");
	fprintf(stream, "\n");
	fprintf(stream, "Options:\n");
	fprintf(stream, " -f file\tread from a file\n");
	fprintf(stream, " -p pipe\tread from a named pipe\n");
	fprintf(stream, " --\t\tstop option scanning\n");
	fprintf(stream, "\n");
}

static bool handle_signals(void)
{
	struct sigaction action =
	{
		.sa_handler = stop_multiplexing
	};

	int signals[] = {SIGINT, SIGHUP, SIGTERM};

	for (size_t index = 0; index < sizeof signals/sizeof *signals; index++)
	{
		if (sigaction(signals[index], &action, NULL) == -1)
		{
			perror("handle_signals: sigaction");
			return false;
		}
	}

	return true;
}

static bool reconfigure_ctty(void)
{
	if (tcgetattr(STDIN_FILENO, &ctty.attributes) == -1)
	{
		perror("reconfigure_ctty: tcgetattr");
		return false;
	}

	if (atexit(reset_ctty))
	{
		fprintf(stderr, "reconfigure_ctty: atexit\n");
		return false;
	}

	struct termios attributes = ctty.attributes;

	attributes.c_iflag &= ~(IGNBRK|BRKINT|PARMRK|ISTRIP
				|INLCR|IGNCR|ICRNL|IXON);
	attributes.c_oflag &= ~OPOST;
	attributes.c_lflag &= ~(ECHO|ECHONL|ICANON|ISIG|IEXTEN);
	attributes.c_cflag &= ~(CSIZE|PARENB);
	attributes.c_cflag |= CS8;

	if (tcsetattr(STDIN_FILENO, TCSANOW, &attributes) == -1)
	{
		perror("reconfigure_ctty: tcsetattr");
		return false;
	}

	return true;
}

static void reset_ctty(void)
{
	if (tcsetattr(STDIN_FILENO, TCSANOW, &ctty.attributes) == -1)
	{
		perror("reset_ctty: tcsetattr");
	}
}

static bool open_pty(void)
{
	pty.master = open_pty_master();

	if (pty.master == -1)
	{
		return false;
	}

	pty.slave = open_pty_slave(pty.master);

	if (pty.slave == -1)
	{
		return false;
	}

	return true;
}

static int open_pty_master(void)
{
	int descriptor = posix_openpt(O_RDWR | O_NOCTTY);

	if (descriptor == -1)
	{
		perror("open_pty_master: posix_openpt");
		return -1;
	}

	if (grantpt(descriptor) == -1)
	{
		perror("open_pty_master: grantpt");
		close(descriptor);
		return -1;
	}

	if (unlockpt(descriptor) == -1)
	{
		perror("open_pty_master: unlockpt");
		close(descriptor);
		return -1;
	}

	return descriptor;
}

static int open_pty_slave(int master)
{
	int descriptor = open(ptsname(master), O_RDWR);
 
	if (descriptor == -1)
	{
		perror("open_pty_slave: open");
		close(descriptor);
		return -1;
	}

	return descriptor;
}

static bool reconfigure_stdio(void)
{
	close(STDIN_FILENO);
	dup2(pty.slave, STDIN_FILENO);

	close(STDOUT_FILENO);
	dup2(pty.slave, STDOUT_FILENO);

	close(STDERR_FILENO);
	dup2(pty.slave, STDERR_FILENO);

	return true;
}

static void track_descriptor(int descriptor)
{
	if (!FD_ISSET(descriptor, &inputs.descriptors))
	{
		FD_SET(descriptor, &inputs.descriptors);
	}

	if (inputs.highest_descriptor < descriptor)
	{
		inputs.highest_descriptor = descriptor;
	}
}

static void untrack_descriptor(int descriptor)
{
	if (FD_ISSET(descriptor, &inputs.descriptors))
	{
		FD_CLR(descriptor, &inputs.descriptors);
	}

	if (inputs.highest_descriptor == descriptor)
	{
		find_highest_descriptor();
	}
}

static void find_highest_descriptor(void)
{
	struct iix_input *input = inputs.tail;

	while (input)
	{
		if (input->descriptor > inputs.highest_descriptor)
		{
			if (FD_ISSET(input->descriptor, &inputs.descriptors))
			{
				inputs.highest_descriptor = input->descriptor;
			}
		}

		input = input->previous;
	}
}

static bool enable_blocking(int descriptor, bool enable)
{
	int flags = fcntl(descriptor, F_GETFL);

	if (flags == -1)
	{
		perror("enable_blocking: fcntl: F_GETFL");
		return false;
	}

	flags = enable ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);

	if (fcntl(descriptor, F_SETFL, flags) == -1)
	{
		perror("enable_blocking: fcntl: F_SETFL");
		return false;
	}

	return true;
}

static bool add_standard_input(void)
{
	int descriptor = STDIN_FILENO;

	if (!enable_blocking(descriptor, false))
	{
		return false;
	}

	return add_input(descriptor);
}

static bool add_file_input(const char *filename)
{
	int descriptor = open(filename, O_RDONLY|O_NONBLOCK, 0);

	if (descriptor == -1)
	{
		perror("add_file_input: open");
		return false;
	}

	return add_input(descriptor);
}

static bool add_pipe_input(const char *filename)
{
	struct stat status;

	if (stat(filename, &status) == -1)
	{
		perror("add_pipe_input: stat");
		return false;
	}

	if (!S_ISFIFO(status.st_mode))
	{
		fprintf(stderr, "add_pipe_input: not a pipe\n");
		return false;
	}

	int descriptor = open(filename, O_RDWR|O_NONBLOCK, 0);

	if (descriptor == -1)
	{
		perror("add_pipe_input: open");
		return false;
	}

	return add_input(descriptor);
}

static bool add_input(int descriptor)
{
	if (!allocate_input(descriptor))
	{
		return false;
	}

	track_descriptor(descriptor);
	return true;
}

static bool allocate_input(int descriptor)
{
	struct iix_input *input = calloc(1, sizeof(struct iix_input));

	if (!input)
	{
		perror("allocate_input: calloc");
		return false;
	}

	input->descriptor = descriptor;
	link_input(input);

	return true;
}

static void remove_input(struct iix_input *input)
{
	if (input->descriptor == STDIN_FILENO)
	{
		enable_blocking(input->descriptor, true);
	}

	else
	{
		close(input->descriptor);
	}

	untrack_descriptor(input->descriptor);
	unlink_input(input);
	free(input);
}

static void remove_inputs(void)
{
	struct iix_input *input = inputs.tail;

	while (input)
	{
		struct iix_input *previous = input->previous;
		remove_input(input);
		input = previous;
	}
}

static void link_input(struct iix_input *input)
{
	if (!inputs.head)
	{
		inputs.head = input;
	}

	if (!inputs.tail)
	{
		inputs.tail = input;
	}

	else
	{
		inputs.tail->next = input;
		input->previous = inputs.tail;
		inputs.tail = input;
	}
}

static void unlink_input(struct iix_input *input)
{
	if (input->previous)
	{
		input->previous->next = input->next;
	}

	if (input->next)
	{
		input->next->previous = input->previous;
	}

	if (input == inputs.head)
	{
		inputs.head = input->next;
	}

	if (input == inputs.tail)
	{
		inputs.tail = input->previous;
	}
}

static bool execute_program(void)
{
	if (!reconfigure_stdio())
	{
		return false;
	}

	close(pty.slave);
	cleanup();

	if (setsid() == -1)
	{
		perror("execute_program: setsid");
		return false;
	}

	if (execvp(program.name, program.arguments) == -1)
	{
		perror("execute_program: execvp");
		return false;
	}

	return true;
}

static bool multiplex(void)
{
	if (!add_standard_input())
	{
		return false;
	}

	if (!add_input(pty.master))
	{
		return false;
	}

	multiplexing = true;

	while (multiplexing)
	{
		struct timeval timeout = {
			.tv_sec = 1,
			.tv_usec = 0
		};

		fd_set active = inputs.descriptors;

		int pending = select(inputs.highest_descriptor + 1,
				     &active, NULL, NULL, &timeout);

		if (pending == -1)
		{
			if (errno != EINTR)
			{
				perror("multiplex: select");
			}

			stop_multiplexing();
			return false;
		}

		if (pending == 0)
		{
			continue;
		}

		struct iix_input *input = inputs.head;

		while (input)
		{
			struct iix_input *next = input->next;

			if (FD_ISSET(input->descriptor, &active))
			{
				if (!service(input, pty.master))
				{
					stop_multiplexing();
					break;
				}
			}

			input = next;
		}
	}

	return true;
}

static bool service(struct iix_input *input, int output)
{
	int descriptor = input->descriptor;

	if (descriptor == output)
	{
		output = STDOUT_FILENO;
	}

	unsigned char buffer[BUFSIZ] = {0};
	ssize_t pending = read(descriptor, buffer, sizeof buffer);

	if (pending == -1)
	{
		return false;
	}

	else if (pending == 0)
	{
		if (descriptor == STDIN_FILENO)
		{
			return false;
		}

		remove_input(input);
		return true;
	}

	while (pending)
	{
		ssize_t written = write(output, buffer, pending);

		if (written == -1)
		{
			return false;
		}

		pending -= written;
	}

	return true;
}

static void stop_multiplexing()
{
	multiplexing = false;
}
