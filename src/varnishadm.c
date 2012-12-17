#include "config.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <pthread.h>


#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vapi/vsl.h"
#include "vapi/vsm.h"
#include "vas.h"
#include "vcli.h"
#include "vss.h"

#include "plugins.h"
#include "varnishadm.h"
#include "main.h"
#include "ipc.h"

#define RL_EXIT(s) exit(s)


static int
cli_write(int sock, const char *s)
{
	int i, l;

	i = strlen(s);
	l = write (sock, s, i);
	if (i == l)
		return 1;
	perror("Write error CLI socket");
	AZ(close(sock));
	return 0;
}

/*
 * This function establishes a connection to the specified ip and port and
 * sends a command to varnishd. If varnishd returns an OK status, the result
 * is printed and 0 returned. Else, an error message is printed and 1 is
 * returned
 */
static int
cli_sock(struct vadmin_config_t *vadmin)
{
	int fd;
	unsigned status;
	char *answer = NULL;
	char buf[CLI_AUTH_RESPONSE_LEN + 1];

	vadmin->sock = VSS_open(vadmin->T_arg, vadmin->timeout);
	if (vadmin->sock < 0) {
		fprintf(stderr, "Connection failed (%s)\n", vadmin->T_arg);
		return (-1);
	}

	(void)VCLI_ReadResult(vadmin->sock, &status, &answer, vadmin->timeout);
	if (status == CLIS_AUTH) {
		if (vadmin->S_arg == NULL) {
			fprintf(stderr, "Authentication required\n");
			AZ(close(vadmin->sock));
			return(-1);
		}
		fd = open(vadmin->S_arg, O_RDONLY);
		if (fd < 0) {
			fprintf(stderr, "Cannot open \"%s\": %s\n",
			    vadmin->S_arg, strerror(errno));
			AZ(close(vadmin->sock));
			return (-1);
		}
		VCLI_AuthResponse(fd, answer, buf);
		AZ(close(fd));
		free(answer);

		cli_write(vadmin->sock, "auth ");
		cli_write(vadmin->sock, buf);
		cli_write(vadmin->sock, "\n");
		(void)VCLI_ReadResult(vadmin->sock, &status, &answer, vadmin->timeout);
	}
	if (status != CLIS_OK) {
		fprintf(stderr, "Rejected %u\n%s\n", status, answer);
		AZ(close(vadmin->sock));
		return (-1);
	}
	free(answer);

	cli_write(vadmin->sock, "ping\n");
	(void)VCLI_ReadResult(vadmin->sock, &status, &answer, vadmin->timeout);
	if (status != CLIS_OK || strstr(answer, "PONG") == NULL) {
		fprintf(stderr, "No pong received from server\n");
		AZ(close(vadmin->sock));
		return(-1);
	}
	free(answer);

	return (vadmin->sock);
}

void
vadmin_run(int sock, char *cmd, struct ipc_ret_t *ret)
{
	assert(cmd);
	cli_write(sock, cmd);
	cli_write(sock, "\n");

	(void)VCLI_ReadResult(sock, &ret->status, &ret->answer, 2000);
}

void
read_cmd(void *private, char *msg, struct ipc_ret_t *ret)
{
	struct vadmin_config_t *vadmin = (struct vadmin_config_t *) private;
	vadmin_run(vadmin->sock, msg, ret);
}

static int
n_arg_sock(struct vadmin_config_t *vadmin)
{
	char *T_start;
	struct VSM_data *vsd;
	char *p;
	struct VSM_fantom vt;

	vsd = VSM_New();
	assert(VSL_Arg(vsd, 'n', vadmin->n_arg));
	if (VSM_Open(vsd)) {
		fprintf(stderr, "%s\n", VSM_Error(vsd));
		return (-1);
	}

	if (!VSM_Get(vsd, &vt, "Arg", "-T", "")) {
		fprintf(stderr, "No -T arg in shared memory\n");
		return (-1);
	}
	AN(vt.b);
	T_start = vadmin->T_arg = strdup(vt.b);

	if (VSM_Get(vsd, &vt, "Arg", "-S", "")) {
		AN(vt.b);
		vadmin->S_arg = strdup(vt.b);
	}

	
	p = strchr(vadmin->T_arg, '\n');
	AN(p);
	*p = '\0';
	return (1);
}

void
vadmin_preconf(struct agent_core_t *core)
{
	struct vadmin_config_t *vadmin;
	struct agent_plugin_t *v;
	v  = plugin_find(core, "vadmin");
	v->ipc->cb = read_cmd;
	vadmin = malloc(sizeof(struct vadmin_config_t ));
	v->data = vadmin;
	v->ipc->priv = vadmin;
	v->start = ipc_start;
	
	vadmin->n_arg = NULL;
	vadmin->S_arg = NULL;
	vadmin->T_arg = NULL;
	vadmin->timeout = 5;
	vadmin->sock = -1;
	
}

pthread_t *
vadmin_start(struct agent_core_t *core, char *name)
{
	return ipc_start(core, name);
}

int
vadmin_init(struct agent_core_t *core)
{
	struct vadmin_config_t *vadmin = (struct vadmin_config_t *)core->vadmin;
	if (vadmin->n_arg != NULL) {
		if (vadmin->T_arg != NULL || vadmin->S_arg != NULL) {
			return -1;
		}
		n_arg_sock(vadmin);
	} else if (vadmin->T_arg == NULL) {
		vadmin->n_arg = "";
		n_arg_sock(vadmin);
	} else {
		assert(vadmin->T_arg != NULL);
	}
	cli_sock(vadmin);
	if (vadmin->sock < 0)
		exit(2);
		
	return (1);
}