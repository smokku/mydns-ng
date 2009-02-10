/**************************************************************************************************

	Copyright (C) 2007-2009  Howard Wilkinson <howard@cohtech.com>

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at Your option) any later version.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program; if not, write to the Free Software
	Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
**************************************************************************************************/

#include "named.h"

#include "memoryman.h"

#include "debug.h"
#include "server.h"
#include "support.h"
#include "taskobj.h"

#include "servercomms.h"
#include "task.h"

#if DEBUG_ENABLED
int		debug_servercomms = 0;
#endif

#define KEEPALIVE 30

typedef struct _named_message {
  uint16_t	messagelength;
  uint16_t	messageid;
  char		messagedata[4]; /* This is a variable length field in reality */
} COMMSMESSAGE;

typedef struct _named_comms {
  COMMSMESSAGE	*message;
  int		donesofar;
  time_t	connectionalive;
  void		*data;
} COMMS;

typedef int (*CommandProcessor)(TASK *, char *, void *);

typedef struct _command {
  const char		*commandname;
  CommandProcessor	commandproc;
} COMMAND;

static int messageid = 0;

static int comms_sendping(TASK *, char *, void *);
static int comms_sendpong(TASK *, char *, void *);

/* Commands from the master to the server */
static COMMAND servercommands[] = { { "STOP AXFR",	NULL },
				    { "START AXFR",	NULL },
				    { "FLUSH ZONE",	NULL },
				    { "FLUSH",		NULL },
				    { "RELOAD ZONE",	NULL },
				    { "RELOAD",		NULL },
				    { "SEND STATS",	NULL },
				    { "PING",		comms_sendpong },
				    { "PONG",		NULL },
				    { NULL,		NULL } };

static COMMAND mastercommands[] = { { "STATS",		NULL },
				    { "TCP STOP",	NULL },
				    { "TCP START",	NULL },
				    { "STOPPED AXFR",	NULL },
				    { "STARTED AXFR",	NULL },
				    { "FLUSHED ZONE",	NULL },
				    { "FLUSHED",	NULL },
				    { "RELOADED ZONE",	NULL },
				    { "RELOADED",	NULL },
				    { "PING",		comms_sendpong },
				    { "PONG",		NULL },
				    { NULL,		NULL } };

static COMMS *
__comms_allocate(void) {
  COMMS		*comms = NULL;

  comms = ALLOCATE(sizeof(COMMS), COMMS*);

  comms->message = NULL;
  comms->donesofar = 0;
  comms->connectionalive = current_time;
  comms->data = NULL;
  return comms;
}

static COMMSMESSAGE *
__message_allocate(size_t messagelength) {
  COMMSMESSAGE	*message = NULL;

  messagelength += sizeof(COMMSMESSAGE) - sizeof(message->messagedata) + 1;
  message = ALLOCATE(messagelength, COMMSMESSAGE*);

  message->messageid = htons(messageid++);
  message->messagelength = htons(messagelength);
  return message;
}

static COMMSMESSAGE *
_message_allocate(char *commandstring) {
  COMMSMESSAGE	*message = NULL;

  message = __message_allocate(strlen(commandstring));

  strncpy(&message->messagedata[0], commandstring, strlen(commandstring));
  message->messagedata[strlen(commandstring)] = '\0';
  return message;
}

static void
__message_free(TASK *t, void *data) {
  COMMSMESSAGE	*message = (COMMSMESSAGE*)data;

  if (!message) return;
}

static void
__comms_free(TASK *t, void *data) {
  COMMS		*comms = (COMMS*)data;

  if (!comms) return;

  __message_free(t, comms->message);
  RELEASE(comms->message);
}

static int
_comms_recv(TASK *t, void *data, size_t size, int flags) {
  int	rv  = 0;

  rv = recv(t->fd, data, size, MSG_DONTWAIT|flags);

  if (rv > 0) return rv;
  if (rv == 0) {
    /* Treat as the same as a stall
       Warn("%s: %s", desctask(t), _("connection closed assume it died")); */
    return 0;
  }

  if (
      (errno == EINTR)
#ifdef EAGAIN
      || (errno == EAGAIN)
#else
#ifdef EWOULDBLOCK
      || (errno == EWOULDBLOCK)
#endif
#endif
      ) {
    return 0; /* Try again later */
  }
  Warn("%s: %s (%d) - %s(%d)", desctask(t), _("Receive failed"), rv, strerror(errno), errno);

  return rv;
}

static int
_comms_send(TASK *t, void *data, size_t size, int flags) {
  int rv = 0;

#if !defined(MSG_NOSIGNAL)
#define __FAKE_MSG_NOSIGNAL__
#define MSG_NOSIGNAL 0
#if defined(SO_NOSIGPIPE)
#define __USE_SO_NOSIGPIPE__
  {
    int on = 1;
    setsockopt(t->fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
#endif

  rv = send(t->fd, data, size, MSG_DONTWAIT|MSG_NOSIGNAL|flags);

#if defined(__FAKE_MSG_NOSIGNAL__)
#undef __FAKE_MSG_NOSIGNAL__
#if defined(__USE_SO_NOSIGPIPE__)
#undef __USE_SO_NOSIGPIPE__
    on = 0;
    setsockopt(t->fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
  }
#endif
#endif

  if (rv > 0) return rv;

  if (rv == 0) {
      Warn("%s: %s", desctask(t), _("connection closed assume it died"));
      return -2;
  }
   
  if (
      (errno == EINTR)
#ifdef EAGAIN
      || (errno == EAGAIN)
#else
#ifdef EWOULDBLOCK
      || (errno == EWOULDBLOCK)
#endif
#endif
      ) {
    return 0; /* Try again later */
  }
  Warn("%s: %s - %s", desctask(t), _("Send failed"), strerror(errno));

  return rv;
}

static taskexec_t
comms_recv(TASK *t, COMMS *comms) {
  int		rv = 0;
  uint16_t	messagelength = 0;
  char		*msgbuf = NULL;

  if (comms->message == NULL) {
    /* Read in message and dispatch */
    rv = _comms_recv(t, &messagelength, sizeof(messagelength), MSG_PEEK);

    if (rv == -1) { return TASK_FAILED; }

    if (rv != sizeof(messagelength)) { return TASK_CONTINUE; }

    messagelength = ntohs(messagelength);
    comms->message = __message_allocate(messagelength);
    comms->donesofar = 0;
  } else {
    messagelength = ntohs(comms->message->messagelength);
  }

  msgbuf = (char*)comms->message;
  msgbuf = &msgbuf[comms->donesofar];
  messagelength -= comms->donesofar;

  while (messagelength > 0) {
    rv = _comms_recv(t, msgbuf, messagelength, 0);
    if (rv == 0) { /* STALLED */ return TASK_CONTINUE; }
    if (rv == -1) {
      __comms_free(t, comms);
      return TASK_FAILED;
    }
    messagelength -= rv;
    msgbuf = &msgbuf[rv];
    comms->donesofar += rv;
    comms->connectionalive = current_time;
  }

  comms->connectionalive = current_time;

  t->timeout = current_time + KEEPALIVE;

  return TASK_EXECUTED;
}

static taskexec_t
comms_send(TASK *t, void *commsp) {
  COMMS		*comms = commsp;
  int		rv = 0;
  char		*msgbuf = NULL;
  uint16_t	messagelength = 0;
  TASK		*newt = NULL;;

  if (!comms->message) return (TASK_COMPLETED);

  if (!comms->donesofar) {
    newt = IOtask_init(NORMAL_PRIORITY_TASK, NEED_COMMAND_WRITE, t->fd, t->protocol, t->family, NULL);
    task_add_extension(newt, comms, __comms_free, comms_send, NULL);
    newt->timeout = current_time + task_timeout;
  } else {
    newt = t;
  }

  msgbuf = (char*)comms->message;
  messagelength = ntohs(comms->message->messagelength);
  msgbuf = &msgbuf[comms->donesofar];
  messagelength -= comms->donesofar;


  while (messagelength > 0) {
    rv = _comms_send(t, msgbuf, messagelength, 0);

    if (rv == 0) /* Let the task scheduler run us when there is message space */
      return (TASK_CONTINUE);

    if (rv < 0) {
      if (t == newt) {
	return (TASK_FAILED);
      } else {
	dequeue(newt);
	return (TASK_CONTINUE);
      }
    }

    comms->donesofar += rv;
    messagelength -= rv;
  }

  if (t == newt) {
    return (TASK_COMPLETED);
  } else {
    dequeue(newt);
    return (TASK_CONTINUE);
  }
}

static CommandProcessor
comms_find_command(TASK *t, COMMS *comms, COMMAND *commands, char **args) {
  CommandProcessor	action = NULL;
  int			i = 0;
  size_t		commandlength = (ntohs(comms->message->messagelength )
					 - (sizeof(comms->message->messagelength)
					    + sizeof(comms->message->messageid)));
  for (i = 0; commands[i].commandname; i++) {
    if (strlen(commands[i].commandname) > commandlength) continue;
    if (strncasecmp(commands[i].commandname, comms->message->messagedata,
		    strlen(commands[i].commandname))) continue;
    action = commands[i].commandproc;
    *args = &comms->message->messagedata[strlen(commands[i].commandname)];
    break;
  }

  return action;
}

static TASK *
comms_start(int fd, FreeExtension comms_freeer, RunExtension comms_runner,
	    TimeExtension comms_ticker) {
  TASK		*listener = NULL;
  COMMS		*comms = __comms_allocate();

  listener = IOtask_init(NORMAL_PRIORITY_TASK, NEED_COMMAND_READ, fd, SOCK_DGRAM, AF_UNIX, NULL);
  task_add_extension(listener, comms, comms_freeer, comms_runner, comms_ticker);
  listener->timeout = current_time + KEEPALIVE;

  return listener;
}

static taskexec_t
comms_run(TASK *t, void * data) {
  taskexec_t		rv = TASK_FAILED;
  COMMS			*comms = NULL;
  CommandProcessor	action = NULL;
  char			*args = NULL;

  t->timeout = current_time + KEEPALIVE;

  comms = (COMMS*)data;

  rv = comms_recv(t, comms);
#if DEBUG_ENABLED
  Debug(servercomms, 1, _("%s: Received command %s - result %s"), desctask(t),
	 &comms->message->messagedata[0], task_exec_name(rv));
#endif
  if ((rv == TASK_FAILED) || (rv == TASK_CONTINUE)) return TASK_CONTINUE;

  /* Got a message from the master dispatch it. */
  action = comms_find_command(t, comms, servercommands, &args);

  __comms_free(t, comms);
  comms->donesofar = 0;

  if (action)
    action(t, (void*)comms, args);

  return TASK_CONTINUE;
}

static taskexec_t
comms_sendcommand(TASK *t, char *commandstring) {
  COMMS		*comms = NULL;
  taskexec_t	rv = TASK_FAILED;

#if DEBUG_ENABLED
  Debug(servercomms, 1, _("%s: Sending commands %s"), desctask(t), commandstring);
#endif

  comms = __comms_allocate();
  /* Send a command/response */
  comms->message = _message_allocate(commandstring);;

  rv = comms_send(t, comms);
#if DEBUG_ENABLED
  Debug(servercomms, 1, _("%s: Sent command %s - result %s"),
	 desctask(t), commandstring, task_exec_name(rv));
#endif
  return rv;
}

static taskexec_t
scomms_tick(TASK *t, void *data) {
  taskexec_t	rv = TASK_CONTINUE;
  COMMS		*comms = (COMMS*)data;
  int		lastseen = current_time - comms->connectionalive;

  t->timeout = current_time + KEEPALIVE;

  if (lastseen <= KEEPALIVE) return TASK_CONTINUE;

  if (lastseen  <= (5*KEEPALIVE))
    rv = comms_sendping(t, NULL, NULL);
  else
    rv = TASK_FAILED;

  if (rv == TASK_FAILED) {
    /* Nothing from the master for 5 cycles assume one of us has gone AWOL */
#if DEBUG_ENABLED
    Debug(servercomms, 1, _("%s: Server comms tick - master has not pinged for %d seconds"),
	   desctask(t),
	   lastseen);
#endif
    named_shutdown(0);
  }

  return TASK_CONTINUE;
}

static int
mcomms_tick(TASK *t, void *data) {
  taskexec_t	rv = TASK_CONTINUE;
  COMMS		*comms = (COMMS*)data;
  int		lastseen = current_time - comms->connectionalive;

  t->timeout = current_time + KEEPALIVE;

  if (lastseen <= KEEPALIVE) return TASK_CONTINUE;

  if (lastseen <= (5*KEEPALIVE))
    rv = comms_sendping(t, NULL, NULL);
  else
    rv = TASK_FAILED;

  if (rv == TASK_FAILED) {
    /* Shutdown and restart server at other end of connection */
    SERVER *server = server_find_by_task(t);
#if DEBUG_ENABLED
    Debug(servercomms, 1,
	   _("%s: Master comms tick - connection to server has not pinged for %d seconds"),
	   desctask(t), lastseen);
#endif
    if (server) {
      if (server->signalled) {
	server->signalled = SIGKILL;
      } else {
	server->signalled = SIGTERM;
      }
      server_kill(server, server->signalled);
      t->timeout = current_time + 5; /* Give the server time to die */
      rv = TASK_CONTINUE;
    }
  }

  return TASK_CONTINUE;
}

TASK *
scomms_start(int fd) {
  return comms_start(fd, __comms_free, comms_run, scomms_tick);
}

TASK *
mcomms_start(int fd) {
  return comms_start(fd, __comms_free, comms_run, mcomms_tick);
}


static taskexec_t
comms_sendping(TASK *t, char *comms, void *args) {
  int		rv;

  rv = comms_sendcommand(t, (char*)"PING");

  return rv;
}

static taskexec_t
comms_sendpong(TASK *t, char *comms, void *args) {
  int		rv;

  rv = comms_sendcommand(t, (char*)"PONG");

  return rv;
}

/* vi:set ts=3: */
/* NEED_PO */