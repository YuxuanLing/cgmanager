/* cgmanager
 *
 * Copyright © 2013 Stphane Graber
 * Author: Stphane Graber <stgraber@ubuntu.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2, as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sched.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/param.h>
#include <stdbool.h>
#include <libgen.h>
#include <unistd.h>
#include <sys/mount.h>
#include <dirent.h>

#include <nih/macros.h>
#include <nih/alloc.h>
#include <nih/string.h>
#include <nih/io.h>
#include <nih/option.h>
#include <nih/main.h>
#include <nih/logging.h>
#include <nih/error.h>

#include <nih-dbus/dbus_connection.h>
#include <nih-dbus/dbus_object.h>
#include <nih-dbus/dbus_proxy.h>
#include <nih-dbus/dbus_error.h>

#include <sys/socket.h>

#include "cgmanager.h"
#include "fs.h"
#include "access_checks.h"
#include "org.linuxcontainers.cgmanager.h"

#include "config.h"

/**
 * daemonise:
 *
 * Set to TRUE if we should become a daemon, rather than just running
 * in the foreground.
 **/
static int daemonise = FALSE;

bool setns_pid_supported = false;
unsigned long mypidns;
bool setns_user_supported = false;
unsigned long myuserns;

int cgmanager_ping (void *data, NihDBusMessage *message, int junk)
{
	if (message == NULL) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
			"message was null");
		return -1;
	}

	return 0;
}

struct scm_sock_data {
	int type;
	char *controller;
	char *cgroup;
	char *key;
	char *value;
	int step;
	struct ucred rcred, vcred;
	int fd;
	int recursive;
};

enum req_type {
	REQ_TYPE_GET_PID,
	REQ_TYPE_MOVE_PID,
	REQ_TYPE_CREATE,
	REQ_TYPE_CHOWN,
	REQ_TYPE_GET_VALUE,
	REQ_TYPE_SET_VALUE,
	REQ_TYPE_REMOVE,
	REQ_TYPE_GET_TASKS,
};

int get_pid_cgroup_main(const void *parent, const char *controller,
		struct ucred r, struct ucred v, char **output);
void get_pid_scm_complete(struct scm_sock_data *data);
int move_pid_main(const char *controller, const char *cgroup,
		struct ucred r, struct ucred v);
void move_pid_scm_complete(struct scm_sock_data *data);
int create_main(const char *controller, const char *cgroup,
		struct ucred ucred, int32_t *existed);
void create_scm_complete(struct scm_sock_data *data);
int chown_main(const char *controller, const char *cgroup,
		struct ucred r, struct ucred v);
void chown_scm_complete(struct scm_sock_data *data);
int get_value_main(void *parent, const char *controller,
		const char *req_cgroup, const char *key, struct ucred ucred,
		char **value);
void get_value_complete(struct scm_sock_data *data);
int set_value_main(const char *controller, const char *req_cgroup,
		const char *key, const char *value, struct ucred ucred);
void set_value_complete(struct scm_sock_data *data);
int remove_main(const char *controller, const char *cgroup, struct ucred ucred,
		 int recursive, int32_t *existed);
void remove_scm_complete(struct scm_sock_data *data);
int get_tasks_main (void *parent, const char *controller, const char *cgroup,
			struct ucred ucred, int32_t **pids);
void get_tasks_scm_complete(struct scm_sock_data *data);

static struct scm_sock_data *alloc_scm_sock_data(int fd, enum req_type t)
{
	struct scm_sock_data *d;
	int optval = -1;

	if (setsockopt(fd, SOL_SOCKET, SO_PASSCRED, &optval, sizeof(optval)) == -1) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
				"Failed to set passcred: %s", strerror(errno));
		return NULL;
	}
	d = nih_alloc(NULL, sizeof(*d));
	if (!d) {
		nih_dbus_error_raise_printf (DBUS_ERROR_NO_MEMORY,
			"Out of memory");
		return NULL;
	}
	memset(d, 0, sizeof(*d));
	d->fd = fd;
	d->type = t;
	return d;
}

static bool need_two_creds(enum req_type t)
{
	switch (t) {
	case REQ_TYPE_GET_PID:
	case REQ_TYPE_MOVE_PID:
	case REQ_TYPE_CHOWN:
		return true;
	default:
		return false;
	}
}

static void
scm_sock_error_handler (void *data, NihIo *io)
{
	struct scm_sock_data *d = data;
	NihError *error = nih_error_get ();
	nih_error("got an error, type %d", d->type);
	nih_error("error %s", strerror(error->number));
	nih_free(error);
}

static void
scm_sock_close (struct scm_sock_data *data, NihIo *io)
{
	nih_assert (data);
	nih_assert (io);
	close (data->fd);
	nih_free (data);
	nih_free (io);
}

static bool kick_fd_client(int fd)
{
	char buf = '1';
	if (write(fd, &buf, 1) != 1) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
			"Failed to start write on scm fd: %s", strerror(errno));
		return false;
	}
	return true;
}

void sock_scm_reader(struct scm_sock_data *data,
			NihIo *io, const char *buf, size_t len)
{
	struct ucred ucred;

	if (!get_nih_io_creds(io, &ucred)) {
		nih_error("failed to read ucred");
		nih_io_shutdown(io);
		return;
	}
	if (data->step == 0) {
		memcpy(&data->rcred, &ucred, sizeof(struct ucred));
		if (need_two_creds(data->type)) {
			data->step = 1;
			if (!kick_fd_client(data->fd))
				nih_io_shutdown(io);
			return;
		}
	} else
		memcpy(&data->vcred, &ucred, sizeof(struct ucred));

	switch (data->type) {
	case REQ_TYPE_GET_PID: get_pid_scm_complete(data); break;
	case REQ_TYPE_MOVE_PID: move_pid_scm_complete(data); break;
	case REQ_TYPE_CREATE: create_scm_complete(data); break;
	case REQ_TYPE_CHOWN: chown_scm_complete(data); break;
	case REQ_TYPE_GET_VALUE: get_value_complete(data); break;
	case REQ_TYPE_SET_VALUE: set_value_complete(data); break;
	case REQ_TYPE_REMOVE: remove_scm_complete(data); break;
	case REQ_TYPE_GET_TASKS: get_tasks_scm_complete(data); break;
	default:
		nih_fatal("%s: bad req_type %d", __func__, data->type);
		exit(1);
	}
	nih_io_shutdown(io);
}

/* GetPidCgroup */
int get_pid_cgroup_main (const void *parent, const char *controller,
			 struct ucred r, struct ucred v, char **output)
{
	char rcgpath[MAXPATHLEN], vcgpath[MAXPATHLEN];

	// Get r's current cgroup in rcgpath
	if (!compute_pid_cgroup(r.pid, controller, "", rcgpath)) {
		nih_error("Could not determine the requestor cgroup");
		return -1;
	}

	// Get v's cgroup in vcgpath
	if (!compute_pid_cgroup(v.pid, controller, "", vcgpath)) {
		nih_error("Could not determine the victim cgroup");
		return -1;
	}

	// Make sure v's cgroup is under r's
	int rlen = strlen(rcgpath);
	if (strncmp(rcgpath, vcgpath, rlen) != 0) {
		nih_error("v (%d)'s cgroup is not below r (%d)'s",
			v.pid, r.pid);
		return -1;
	}
	if (strlen(vcgpath) == rlen)
		*output = nih_strdup(parent, "/");
	else
		*output = nih_strdup(parent, vcgpath + rlen + 1);

	if (! *output)
		nih_return_no_memory_error(-1);

	return 0;
}

void get_pid_scm_complete(struct scm_sock_data *data)
{
	char *output = NULL;
	int ret;

	ret = get_pid_cgroup_main(data, data->controller, data->rcred,
				data->vcred, &output);
	if (ret == 0)
		ret = write(data->fd, output, strlen(output)+1);
	else
		ret = write(data->fd, &data->rcred, 0);  // kick the client
	if (ret < 0)
		nih_error("GetPidCgroupScm: Error writing final result to client: %s",
			strerror(errno));
}

/*
 * This is one of the dbus callbacks.
 * Caller requests the cgroup of @pid in a given @controller
 */
int cgmanager_get_pid_cgroup_scm (void *data, NihDBusMessage *message,
			const char *controller, int sockfd)
{
	struct scm_sock_data *d;

	d = alloc_scm_sock_data(sockfd, REQ_TYPE_GET_PID);
	if (!d)
		return -1;
	d->controller = nih_strdup(d, controller);

	if (!nih_io_reopen(NULL, sockfd, NIH_IO_MESSAGE,
		(NihIoReader) sock_scm_reader,
		(NihIoCloseHandler) scm_sock_close,
		 scm_sock_error_handler, d)) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
			"Failed to queue scm message: %s", strerror(errno));
		return -1;
	}

	if (!kick_fd_client(sockfd))
		return -1;
	return 0;
}

/* GetPidCgroup */
/*
 * This is one of the dbus callbacks.
 * Caller requests the cgroup of @pid in a given @controller
 */
int cgmanager_get_pid_cgroup (void *data, NihDBusMessage *message,
			const char *controller, int plain_pid, char **output)
{
	int fd = 0, ret;
	struct ucred rcred, vcred;
	socklen_t len;

	if (message == NULL) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
			"message was null");
		return -1;
	}

	if (!dbus_connection_get_socket(message->connection, &fd)) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
					     "Could not get client socket.");
		return -1;
	}

	len = sizeof(struct ucred);
	NIH_MUST (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &rcred, &len) != -1);

	nih_info (_("GetPidCgroup: Client fd is: %d (pid=%d, uid=%u, gid=%u)"),
			fd, rcred.pid, rcred.uid, rcred.gid);

	if (!is_same_pidns(rcred.pid)) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
				"GetPidCgroup called from non-init namespace");
		return -1;
	}
	vcred.uid = 0;
	vcred.gid = 0;
	vcred.pid = plain_pid;
	ret = get_pid_cgroup_main(message, controller, rcred, vcred, output);
	if (ret) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
				"invalid request");
		return -1;
	}
	return 0;
}

/* MovePid */
/*
 * This is one of the dbus callbacks.
 * Caller requests moving a @pid to a particular cgroup identified
 * by the name (@cgroup) and controller type (@controller).
 */
int move_pid_main (const char *controller, const char *cgroup,
		struct ucred r, struct ucred v)
{
	char rcgpath[MAXPATHLEN], path[MAXPATHLEN];
	FILE *f;

	// verify that ucred.pid may move target pid
	if (!may_move_pid(r.pid, r.uid, v.pid)) {
		nih_error("%d may not move %d", r.pid, v.pid);
		return -1;
	}

	if (cgroup[0] == '/' || cgroup[0] == '.') {
		// We could try to be accomodating, but let's not fool around right now
		nih_error("Bad requested cgroup path: %s", cgroup);
		return -1;
	}

	// Get r's current cgroup in rcgpath
	if (!compute_pid_cgroup(r.pid, controller, "", rcgpath)) {
		nih_error("Could not determine the requested cgroup");
		return -1;
	}
	/* rcgpath + / + cgroup + /tasks + \0 */
	if (strlen(rcgpath) + strlen(cgroup) > MAXPATHLEN - 8) {
		nih_error("Path name too long");
		return -1;
	}
	strcpy(path, rcgpath);
	strncat(path, "/", MAXPATHLEN-1);
	strncat(path, cgroup, MAXPATHLEN-1);
	if (realpath_escapes(path, rcgpath)) {
		nih_error("Invalid path %s", path);
		return -1;
	}
	// is r allowed to descend under the parent dir?
	if (!may_access(r.pid, r.uid, r.gid, path, O_RDONLY)) {
		nih_error("pid %d (uid %u gid %u) may not read under %s",
			r.pid, r.uid, r.gid, path);
		return -1;
	}
	// is r allowed to write to tasks file?
	strncat(path, "/tasks", MAXPATHLEN-1);
	if (!may_access(r.pid, r.uid, r.gid, path, O_WRONLY)) {
		nih_error("pid %d (uid %u gid %u) may not write to %s",
			r.pid, r.uid, r.gid, path);
		return -1;
	}
	f = fopen(path, "w");
	if (!f) {
		nih_error("Failed to open %s", path);
		return -1;
	}
	if (fprintf(f, "%d\n", v.pid) < 0) {
		fclose(f);
		nih_error("Failed to open %s", path);
		return -1;
	}
	if (fclose(f) != 0) {
		nih_error("Failed to write %d to %s", v.pid, path);
		return -1;
	}
	nih_info(_("%d moved to %s:%s by %d's request"), v.pid,
		controller, cgroup, r.pid);
	return 0;
}

void move_pid_scm_complete(struct scm_sock_data *data)
{
	char b = '0';

	if (move_pid_main(data->controller, data->cgroup, data->rcred,
			  data->vcred) == 0)
		b = '1';
	if (write(data->fd, &b, 1) < 0)
		nih_error("MovePidScm: Error writing final result to client");
}

int cgmanager_move_pid_scm (void *data, NihDBusMessage *message,
			const char *controller, const char *cgroup,
			int sockfd)
{
	struct scm_sock_data *d;

	d = alloc_scm_sock_data(sockfd, REQ_TYPE_MOVE_PID);
	if (!d)
		return -1;
	d->controller = nih_strdup(d, controller);
	d->cgroup = nih_strdup(d, cgroup);

	if (!nih_io_reopen(NULL, sockfd, NIH_IO_MESSAGE,
		(NihIoReader) sock_scm_reader,
		(NihIoCloseHandler) scm_sock_close,
		 scm_sock_error_handler, d)) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
			"Failed to queue scm message: %s", strerror(errno));
		return -1;
	}
	if (!kick_fd_client(sockfd))
		return -1;
	return 0;
}

int cgmanager_move_pid (void *data, NihDBusMessage *message,
			const char *controller, const char *cgroup, int plain_pid)
{
	int fd = 0, ret;
	struct ucred ucred, vcred;
	socklen_t len;

	if (message == NULL) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
			"message was null");
		return -1;
	}

	if (!dbus_connection_get_socket(message->connection, &fd)) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
					     "Could not get client socket.");
		return -1;
	}

	len = sizeof(struct ucred);
	NIH_MUST (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &ucred, &len) != -1);

	nih_info (_("MovePid: Client fd is: %d (pid=%d, uid=%u, gid=%u)"),
			fd, ucred.pid, ucred.uid, ucred.gid);

	vcred.uid = 0;
	vcred.gid = 0;
	vcred.pid = plain_pid;
	ret = move_pid_main(controller, cgroup, ucred, vcred);
	if (ret)
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
					     "invalid request");
	return ret;
}

/* 
 * This is one of the dbus callbacks.
 * Caller requests creating a new @cgroup name of type @controller.
 * @name is taken to be relative to the caller's cgroup and may not
 * start with / or .. .
 */
int create_main (const char *controller, const char *cgroup,
		struct ucred ucred, int32_t *existed)
{
	int ret;
	char rcgpath[MAXPATHLEN], path[MAXPATHLEN], dirpath[MAXPATHLEN];
	nih_local char *copy = NULL;
	size_t cgroup_len;
	char *p, *p2, oldp2;

	*existed = 1;
	if (!cgroup || ! *cgroup)  // nothing to do
		return 0;
	if (cgroup[0] == '/' || cgroup[0] == '.') {
		// We could try to be accomodating, but let's not fool around right now
		nih_error("Bad requested cgroup path: %s", cgroup);
		return -1;
	}

	// TODO - support comma-separated list of controllers?  Not sure it's worth it

	// Get r's current cgroup in rcgpath
	if (!compute_pid_cgroup(ucred.pid, controller, "", rcgpath)) {
		nih_error("Could not determine the requested cgroup");
		return -1;
	}

	cgroup_len = strlen(cgroup);

	if (strlen(rcgpath) + cgroup_len > MAXPATHLEN) {
		nih_error("Path name too long");
		return -1;
	}
	copy = nih_strndup(NULL, cgroup, cgroup_len);
	if (!copy) {
		nih_error("Out of memory copying cgroup name");
		return -1;
	}

	strcpy(path, rcgpath);
	strcpy(dirpath, rcgpath);
	for (p=copy; *p; p = p2) {
		*existed = -1;
		for (p2=p; *p2 && *p2 != '/'; p2++);
		oldp2 = *p2;
		*p2 = '\0';
		if (strcmp(p, "..") == 0) {
			nih_error("Out of memory copying cgroup name");
			return -1;
		}
		strncat(path, "/", MAXPATHLEN-1);
		strncat(path, p, MAXPATHLEN-1);
		if (dir_exists(path)) {
			*existed = 1;
			// TODO - properly use execute perms
			if (!may_access(ucred.pid, ucred.uid, ucred.gid, path, O_RDONLY)) {
				nih_error("pid %d (uid %u gid %u) may not look under %s",
					ucred.pid, ucred.uid, ucred.gid, path);
				return -1;
			}
			goto next;
		}
		if (!may_access(ucred.pid, ucred.uid, ucred.gid, dirpath, O_RDWR)) {
			nih_error("pid %d (uid %u gid %u) may not create under %s",
				ucred.pid, ucred.uid, ucred.gid, dirpath);
			return -1;
		}
		ret = mkdir(path, 0755);
		if (ret < 0) {  // Should we ignore EEXIST?  Ok, but don't chown.
			if (errno == EEXIST) {
				*existed = 1;
				goto next;
			}
			nih_error("failed to create %s", path);
			return -1;
		}
		if (!chown_cgroup_path(path, ucred.uid, ucred.gid, true)) {
			nih_error("Failed to change ownership on %s to %u:%u",
				path, ucred.uid, ucred.gid);
			rmdir(path);
			return -1;
		}
		*existed = -1;
next:
		strncat(dirpath, "/", MAXPATHLEN-1);
		strncat(dirpath, p, MAXPATHLEN-1);
		*p2 = oldp2;
		if (*p2)
			p2++;
	}


	nih_info(_("Created %s for %d (%u:%u)"), path, ucred.pid,
		 ucred.uid, ucred.gid);
	return 0;
}

void create_scm_complete(struct scm_sock_data *data)
{
	char b = '0';
	int32_t existed;

	if (create_main(data->controller, data->cgroup, data->rcred, &existed) == 0)
		b = existed == 1 ? '2' : '1';
	if (write(data->fd, &b, 1) < 0)
		nih_error("createScm: Error writing final result to client");
}

int cgmanager_create_scm (void *data, NihDBusMessage *message,
		 const char *controller, const char *cgroup, int sockfd)
{
	struct scm_sock_data *d;

	d = alloc_scm_sock_data(sockfd, REQ_TYPE_CREATE);
	if (!d)
		return -1;
	d->controller = nih_strdup(d, controller);
	d->cgroup = nih_strdup(d, cgroup);

	if (!nih_io_reopen(NULL, sockfd, NIH_IO_MESSAGE,
		(NihIoReader) sock_scm_reader,
		(NihIoCloseHandler) scm_sock_close,
		 scm_sock_error_handler, d)) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
			"Failed to queue scm message: %s", strerror(errno));
		return -1;
	}
	if (!kick_fd_client(sockfd))
		return -1;
	return 0;
}

int cgmanager_create (void *data, NihDBusMessage *message,
			 const char *controller, const char *cgroup, int32_t *existed)
{
	int fd = 0, ret;
	struct ucred ucred;
	socklen_t len;

	*existed = -1;
	if (message == NULL) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
				"message was null");
		return -1;
	}

	if (!dbus_connection_get_socket(message->connection, &fd)) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
				"Could not get client socket.");
		return -1;
	}

	len = sizeof(struct ucred);
	NIH_MUST (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &ucred, &len) != -1);

	nih_info (_("Create: Client fd is: %d (pid=%d, uid=%u, gid=%u)"),
			fd, ucred.pid, ucred.uid, ucred.gid);

	ret = create_main(controller, cgroup, ucred, existed);
	if (ret)
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
				"invalid request");
	nih_info(_("%s: returning %d; existed is %d"), __func__, ret, *existed);
	return ret;
}

/*
 * This is one of the dbus callbacks.
 * Caller requests chowning a cgroup @name in controller @cgroup to a
 * particular @uid.  The uid must be passed in as an scm_cred so the
 * kernel translates it for us.  @r must be root in its own user ns.
 *
 * If we are asked to chown /b to UID, then we will chown:
 * /b itself, /b/tasks, and /b/procs.  Any other files in /b will not be
 * chown.  UID can then create subdirs of /b, but not raise his limits.
 */
int chown_main (const char *controller, const char *cgroup,
		struct ucred r, struct ucred v)
{
	char rcgpath[MAXPATHLEN];
	nih_local char *path = NULL;
	uid_t uid;

	/* If caller is not root in his userns, then he can't chown, as
	 * that requires privilege over two uids */
	if (!hostuid_to_ns(r.uid, r.pid, &uid)|| uid != 0) {
		nih_error("Chown requested by non-root uid %u", r.uid);
		return -1;
	}

	if (cgroup[0] == '/' || cgroup[0] == '.') {
		// We could try to be accomodating, but let's not fool around right now
		nih_error("Bad requested cgroup path: %s", cgroup);
		return -1;
	}

	// Get r's current cgroup in rcgpath
	if (!compute_pid_cgroup(r.pid, controller, "", rcgpath)) {
		nih_error("Could not determine the requested cgroup");
		return -1;
	}
	/* rcgpath + / + cgroup + \0 */
	if (strlen(rcgpath) + strlen(cgroup) > MAXPATHLEN+2) {
		nih_error("Path name too long");
		return -1;
	}
	path = nih_sprintf(NULL, "%s/%s", rcgpath, cgroup);
	if (!path) {
		nih_error("Out of memory calculating pathname");
		return -1;
	}
	if (realpath_escapes(path, rcgpath)) {
		nih_error("Invalid path %s", path);
		return -1;
	}
	// is r allowed to descend under the parent dir?
	if (!may_access(r.pid, r.uid, r.gid, path, O_RDONLY)) {
		nih_error("pid %d (uid %u gid %u) may not read under %s",
			r.pid, r.uid, r.gid, path);
		return -1;
	}

	// does r have privilege over the cgroup dir?
	if (!may_access(r.pid, r.uid, r.gid, path, O_RDWR)) {
		nih_error("Pid %d may not chown %s\n", r.pid, path);
		return -1;
	}

	// go ahead and chown it.
	if (!chown_cgroup_path(path, v.uid, v.gid, false)) {
		nih_error("Failed to change ownership on %s to %u:%u",
			path, v.uid, v.gid);
		return -1;
	}

	return 0;
}

void chown_scm_complete(struct scm_sock_data *data)
{
	char b = '0';

	if (chown_main(data->controller, data->cgroup, data->rcred, data->vcred)
			== 0)
		b = '1';
	if (write(data->fd, &b, 1) < 0)
		nih_error("ChownScm: Error writing final result to client");
}

int cgmanager_chown_scm (void *data, NihDBusMessage *message,
			const char *controller, const char *cgroup, int sockfd)
{
	struct scm_sock_data *d;

	d = alloc_scm_sock_data(sockfd, REQ_TYPE_CHOWN);
	if (!d)
		return -1;
	d->controller = nih_strdup(d, controller);
	d->cgroup = nih_strdup(d, cgroup);

	if (!nih_io_reopen(NULL, sockfd, NIH_IO_MESSAGE,
		(NihIoReader)  sock_scm_reader,
		(NihIoCloseHandler) scm_sock_close,
		 scm_sock_error_handler, d)) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
			"Failed to queue scm message: %s", strerror(errno));
		return -1;
	}
	if (!kick_fd_client(sockfd))
		return -1;
	return 0;
}

int cgmanager_chown (void *data, NihDBusMessage *message,
			const char *controller, const char *cgroup, int uid, int gid)
{
	int fd = 0, ret;
	struct ucred ucred, vcred;
	socklen_t len;

	if (message == NULL) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
			"message was null");
		return -1;
	}

	if (!dbus_connection_get_socket(message->connection, &fd)) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
					     "Could not get client socket.");
		return -1;
	}

	len = sizeof(struct ucred);
	NIH_MUST (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &ucred, &len) != -1);

	nih_info (_("Chown: Client fd is: %d (pid=%d, uid=%u, gid=%u)"),
			fd, ucred.pid, ucred.uid, ucred.gid);

	if (!is_same_pidns(ucred.pid)) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
				"chown called from non-init pid namespace");
		return -1;
	}
	if (!is_same_userns(ucred.pid)) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
				"chown called from non-init user namespace");
		return -1;
	}

	vcred.pid = 0;
	vcred.uid = uid;
	vcred.gid = gid;

	ret = chown_main(controller, cgroup, ucred, vcred);
	if (ret)
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
					     "invalid request");
	return ret;
}

/* 
 * This is one of the dbus callbacks.
 * Caller requests the value of a particular cgroup file.
 * @controller is the controller, @req_cgroup the cgroup name, and @key the
 * file being queried (i.e. memory.usage_in_bytes).  @req_cgroup is relative
 * to the caller's cgroup, unless it begins with '/' or '..'.
 *
 * XXX Should '/' be disallowed, only '..' allowed?  Otherwise callers can't
 * pretend to be the cgroup root which is annoying in itself
 */
int get_value_main (void *parent, const char *controller, const char *req_cgroup,
		const char *key, struct ucred ucred, char **value)
{
	char path[MAXPATHLEN];

	if (!compute_pid_cgroup(ucred.pid, controller, req_cgroup, path)) {
		nih_error("Could not determine the requested cgroup");
		return -1;
	}

	/* Check access rights to the cgroup directory */
	if (!may_access(ucred.pid, ucred.uid, ucred.gid, path, O_RDONLY)) {
		nih_error("Pid %d may not access %s\n", ucred.pid, path);
		return -1;
	}

	/* append the filename */
	if (strlen(path) + strlen(key) + 2 > MAXPATHLEN) {
		nih_error("filename too long for cgroup %s key %s", path, key);
		return -1;
	}

	strncat(path, "/", MAXPATHLEN-1);
	strncat(path, key, MAXPATHLEN-1);

	/* Check access rights to the file itself */
	if (!may_access(ucred.pid, ucred.uid, ucred.gid, path, O_RDONLY)) {
		nih_error("Pid %d may not access %s\n", ucred.pid, path);
		return -1;
	}

	/* read and return the value */
	*value = file_read_string(parent, path);
	if (!*value) {
		nih_error("Failed to read value from %s", path);
		return -1;
	}

	nih_info(_("Sending to client: %s"), *value);
	return 0;
}

void get_value_complete(struct scm_sock_data *data)
{
	char *output = NULL;
	int ret;

	if (!get_value_main(data, data->controller, data->cgroup, data->key,
			data->rcred, &output))
		ret = write(data->fd, output, strlen(output)+1);
	else
		ret = write(data->fd, &data->rcred, 0);  // kick the client
	if (ret < 0)
		nih_error("GetValueScm: Error writing final result to client");
}

int cgmanager_get_value_scm (void *data, NihDBusMessage *message,
				 const char *controller, const char *req_cgroup,
				 const char *key, int sockfd)
{
	struct scm_sock_data *d;

	d = alloc_scm_sock_data(sockfd, REQ_TYPE_GET_VALUE);
	if (!d)
		return -1;
	d->controller = nih_strdup(d, controller);
	d->cgroup = nih_strdup(d, req_cgroup);
	d->key = nih_strdup(d, key);

	if (!nih_io_reopen(NULL, sockfd, NIH_IO_MESSAGE,
		(NihIoReader) sock_scm_reader,
		(NihIoCloseHandler) scm_sock_close,
		 scm_sock_error_handler, d)) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
			"Failed to queue scm message: %s", strerror(errno));
		return -1;
	}
	if (!kick_fd_client(sockfd))
		return -1;
	return 0;

}
int cgmanager_get_value (void *data, NihDBusMessage *message,
				 const char *controller, const char *req_cgroup,
				 const char *key, char **value)

{
	int fd = 0, ret;
	struct ucred ucred;
	socklen_t len;

	if (message == NULL) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
			"Message was NULL");
		return -1;
	}

	if (!dbus_connection_get_socket(message->connection, &fd)) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
					     "Could not get client socket.");
		return -1;
	}

	len = sizeof(struct ucred);
	NIH_MUST (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &ucred, &len) != -1);

	nih_info (_("GetValue: Client fd is: %d (pid=%d, uid=%u, gid=%u)"),
			fd, ucred.pid, ucred.uid, ucred.gid);

	ret = get_value_main(message, controller, req_cgroup, key, ucred, value);
	if (ret)
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
				"invalid request");
	return ret;
}

/* 
 * This is one of the dbus callbacks.
 * Caller requests that a particular cgroup @key be set to @value
 * @controller is the controller, @req_cgroup the cgroup name, and @key the
 * file being queried (i.e. memory.usage_in_bytes).  @req_cgroup is relative
 * to the caller's cgroup.
 */
int set_value_main (const char *controller, const char *req_cgroup,
		const char *key, const char *value, struct ucred ucred)

{
	char path[MAXPATHLEN];

	if (!compute_pid_cgroup(ucred.pid, controller, req_cgroup, path)) {
		nih_error("Could not determine the requested cgroup");
		return -1;
	}

	/* Check access rights to the cgroup directory */
	if (!may_access(ucred.pid, ucred.uid, ucred.gid, path, O_RDONLY)) {
		nih_error("Pid %d may not access %s\n", ucred.pid, path);
		return -1;
	}

	/* append the filename */
	if (strlen(path) + strlen(key) + 2 > MAXPATHLEN) {
		nih_error("filename too long for cgroup %s key %s", path, key);
		return -1;
	}

	strncat(path, "/", MAXPATHLEN-1);
	strncat(path, key, MAXPATHLEN-1);

	/* Check access rights to the file itself */
	if (!may_access(ucred.pid, ucred.uid, ucred.gid, path, O_RDWR)) {
		nih_error("Pid %d may not access %s\n", ucred.pid, path);
		return -1;
	}

	/* read and return the value */
	if (!set_value(path, value)) {
		nih_error("Failed to set value %s to %s", path, value);
		return -1;
	}

	return 0;
}

void set_value_complete(struct scm_sock_data *data)
{
	char b = '0';
	if (set_value_main(data->controller, data->cgroup, data->key,
			data->value, data->rcred) == 0)
		b = '1';
	if (write(data->fd, &b, 1) < 0)
		nih_error("SetValueScm: Error writing final result to client");
}

int cgmanager_set_value_scm (void *data, NihDBusMessage *message,
				 const char *controller, const char *req_cgroup,
				 const char *key, const char *value, int sockfd)
{
	struct scm_sock_data *d;

	d = alloc_scm_sock_data(sockfd, REQ_TYPE_SET_VALUE);
	if (!d)
		return -1;
	d->controller = nih_strdup(d, controller);
	d->cgroup = nih_strdup(d, req_cgroup);
	d->key = nih_strdup(d, key);
	d->value = nih_strdup(d, value);

	if (!nih_io_reopen(NULL, sockfd, NIH_IO_MESSAGE,
		(NihIoReader) sock_scm_reader,
		(NihIoCloseHandler) scm_sock_close,
		 scm_sock_error_handler, d)) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
			"Failed to queue scm message: %s", strerror(errno));
		return -1;
	}
	if (!kick_fd_client(sockfd))
		return -1;
	return 0;
}
int cgmanager_set_value (void *data, NihDBusMessage *message,
				 const char *controller, const char *req_cgroup,
				 const char *key, const char *value)

{
	int fd = 0, ret;
	struct ucred ucred;
	socklen_t len;

	if (message == NULL) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
			"Message was NULL");
		return -1;
	}

	if (!dbus_connection_get_socket(message->connection, &fd)) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
					     "Could not get client socket.");
		return -1;
	}

	len = sizeof(struct ucred);
	NIH_MUST (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &ucred, &len) != -1);

	nih_info (_("SetValue: Client fd is: %d (pid=%d, uid=%u, gid=%u)"),
			fd, ucred.pid, ucred.uid, ucred.gid);

	ret = set_value_main(controller, req_cgroup, key, value, ucred);
	if (ret)
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
					     "invalid request");
	return ret;
}

/*
 * Refuse any '..', and consolidate any '//'
 */
static bool normalize_path(char *path)
{
	if (strstr(path, ".."))
		return false;
	while ((path = strstr(path, "//")) != NULL) {
		char *p2 = path+1;
		while (*p2 == '/')
			p2++;
		memcpy(path, p2, strlen(p2)+1);
		path++;
	}
	return true;
}

/*
 * Recursively delete a cgroup.
 * Cgroup files can't be deleted, but are cleaned up when you remove the
 * containing directory.  A directory cannot be removed until all its
 * children are removed, and can't be removed if any tasks remain.
 *
 * We allow any task which may write under /a/b to delete any cgroups
 * under that, even if, say, it technically is not allowed to remove
 * /a/b/c/d/.
 */
static int recursive_rmdir(char *path)
{
	struct dirent dirent, *direntp;
	DIR *dir;
	char pathname[MAXPATHLEN];
	int failed = 0;

	dir = opendir(path);
	if (!dir) {
		nih_error("Failed to open dir %s for recursive deletion", path);
		return -1;
	}

	while (!readdir_r(dir, &dirent, &direntp)) {
		struct stat mystat;
		int rc;

		if (!direntp)
			break;
		if (!strcmp(direntp->d_name, ".") ||
		    !strcmp(direntp->d_name, ".."))
			continue;
		rc = snprintf(pathname, MAXPATHLEN, "%s/%s", path, direntp->d_name);
		if (rc < 0 || rc >= MAXPATHLEN) {
			failed = 1;
			continue;
		}
		rc = lstat(pathname, &mystat);
		if (rc) {
			failed = 1;
			continue;
		}
		if (S_ISDIR(mystat.st_mode)) {
			if (recursive_rmdir(pathname) < 0)
				failed = 1;
		}
	}

	if (closedir(dir) < 0)
		failed = 1;
	if (rmdir(path) < 0)
		failed = 1;

	return failed ? -1 : 0;
}

/* 
 * This is one of the dbus callbacks.
 * Caller requests creating a new @cgroup name of type @controller.
 * @name is taken to be relative to the caller's cgroup and may not
 * start with / or .. .
 */
int remove_main (const char *controller, const char *cgroup, struct ucred ucred,
		 int recursive, int32_t *existed)
{
	char rcgpath[MAXPATHLEN], path[MAXPATHLEN];
	size_t cgroup_len;
	nih_local char *working = NULL, *copy = NULL, *wcgroup = NULL;
	char *p;

	*existed = 1;
	if (!cgroup || ! *cgroup)  // nothing to do
		return 0;
	if (cgroup[0] == '/' || cgroup[0] == '.') {
		// We could try to be accomodating, but let's not fool around right now
		nih_error("Bad requested cgroup path: %s", cgroup);
		return -1;
	}

	// Get r's current cgroup in rcgpath
	if (!compute_pid_cgroup(ucred.pid, controller, "", rcgpath)) {
		nih_error("Could not determine the requested cgroup");
		return -1;
	}

	cgroup_len = strlen(cgroup);

	if (strlen(rcgpath) + cgroup_len > MAXPATHLEN) {
		nih_error("Path name too long");
		return -1;
	}

	if (!(wcgroup = nih_strdup(NULL, cgroup))) {
		nih_error("Out of memory");
		return -1;
	}
	if (!normalize_path(wcgroup))
		return -1;

	if (!(working = nih_strdup(NULL, rcgpath))) {
		nih_error("Out of memory");
		return -1;
	}
	if (!nih_strcat(&working, NULL, "/")) {
		nih_error("Out of memory");
		return -1;
	}
	if (!nih_strcat(&working, NULL, wcgroup)) {
		nih_error("Out of memory");
		return -1;
	}
	if (!dir_exists(working)) {
		*existed = -1;
		return 0;
	}
	// must have write access to the parent dir
	if (!(copy = nih_strdup(NULL, working)))
		return -1;
	if (!(p = strrchr(copy, '/')))
		return -1;
	*p = '\0';
	if (!may_access(ucred.pid, ucred.uid, ucred.gid, copy, O_WRONLY)) {
		nih_error("pid %d (%u:%u) may not remove %s",
			ucred.pid, ucred.uid, ucred.gid, copy);
		return -1;
	}

	if (!recursive) {
		if (rmdir(working) < 0) {
			nih_error("Failed to remove %s: %s", working, strerror(errno));
			return -1;
		}
	} else if (recursive_rmdir(working) < 0)
			return -1;

	nih_info(_("Removed %s for %d (%u:%u)"), path, ucred.pid,
		 ucred.uid, ucred.gid);
	return 0;
}

void remove_scm_complete(struct scm_sock_data *data)
{
	char b = '0';
	int ret;
	int32_t existed = -1;

	ret = remove_main(data->controller, data->cgroup, data->rcred,
			data->recursive, &existed);
	if (ret == 0)
		b = existed == 1 ? '2' : '1';
	if (write(data->fd, &b, 1) < 0)
		nih_error("removeScm: Error writing final result to client");
}

int cgmanager_remove_scm (void *data, NihDBusMessage *message,
		 const char *controller, const char *cgroup, int recursive, int sockfd)
{
	struct scm_sock_data *d;

	d = alloc_scm_sock_data(sockfd, REQ_TYPE_REMOVE);
	if (!d)
		return -1;
	d->controller = nih_strdup(d, controller);
	d->cgroup = nih_strdup(d, cgroup);
	d->recursive = recursive;

	if (!nih_io_reopen(NULL, sockfd, NIH_IO_MESSAGE,
		(NihIoReader) sock_scm_reader,
		(NihIoCloseHandler) scm_sock_close,
		 scm_sock_error_handler, d)) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
			"Failed to queue scm message: %s", strerror(errno));
		return -1;
	}
	if (!kick_fd_client(sockfd))
		return -1;
	return 0;
}
int cgmanager_remove (void *data, NihDBusMessage *message, const char *controller,
			const char *cgroup, int recursive, int32_t *existed)
{
	int fd = 0, ret;
	struct ucred ucred;
	socklen_t len;

	*existed = -1;
	if (message == NULL) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
			"message was null");
		return -1;
	}

	if (!dbus_connection_get_socket(message->connection, &fd)) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
					     "Could not get client socket.");
		return -1;
	}

	len = sizeof(struct ucred);
	NIH_MUST (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &ucred, &len) != -1);

	nih_info (_("Remove: Client fd is: %d (pid=%d, uid=%u, gid=%u)"),
			fd, ucred.pid, ucred.uid, ucred.gid);

	ret = remove_main(controller, cgroup, ucred, recursive, existed);
	if (ret)
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
					     "invalid request");
	return ret;
}

/* 
 * This is one of the dbus callbacks.
 * Caller requests the number of tasks in @cgroup in @controller
 * returns nrpids, or -1 on error.
 */
int get_tasks_main (void *parent, const char *controller, const char *cgroup,
			struct ucred ucred, int32_t **pids)
{
	char path[MAXPATHLEN];
	const char *key = "tasks";

	if (!cgroup || ! *cgroup)  // nothing to do
		return 0;
	if (!compute_pid_cgroup(ucred.pid, controller, cgroup, path)) {
		nih_error("Could not determine the requested cgroup");
		return -1;
	}

	/* Check access rights to the cgroup directory */
	if (!may_access(ucred.pid, ucred.uid, ucred.gid, path, O_RDONLY)) {
		nih_error("Pid %d may not access %s\n", ucred.pid, path);
		return -1;
	}

	/* append the filename */
	if (strlen(path) + strlen(key) + 2 > MAXPATHLEN) {
		nih_error("filename too long for cgroup %s key %s", path, key);
		return -1;
	}

	strncat(path, "/", MAXPATHLEN-1);
	strncat(path, key, MAXPATHLEN-1);

	return file_read_pids(parent, path, pids);
}

void get_tasks_scm_complete(struct scm_sock_data *data)
{
	struct ucred pcred;
	int i, ret;
	int32_t *pids, nrpids;
	ret = get_tasks_main(data, data->controller, data->cgroup,
			data->rcred, &pids);
	if (ret < 0) {
		nih_error("Error getting nrtasks for %s:%s for pid %d",
			data->controller, data->cgroup, data->rcred.pid);
		return;
	}
	nrpids = ret;
	if (write(data->fd, &nrpids, sizeof(int32_t)) != sizeof(int32_t)) {
		nih_error("get_tasks_scm: Error writing final result to client");
		return;
	}
	pcred.uid = 0; pcred.gid = 0;
	for (i=0; i<ret; i++) {
		pcred.pid = pids[i];
		if (send_creds(data->fd, &pcred)) {
			nih_error("get_tasks_scm: error writing pids back to client");
			return;
		}
	}
}

int cgmanager_get_tasks_scm (void *data, NihDBusMessage *message,
		 const char *controller, const char *cgroup, int sockfd)
{
	struct scm_sock_data *d;

	d = alloc_scm_sock_data(sockfd, REQ_TYPE_GET_TASKS);
	if (!d)
		return -1;
	d->controller = nih_strdup(d, controller);
	d->cgroup = nih_strdup(d, cgroup);

	if (!nih_io_reopen(NULL, sockfd, NIH_IO_MESSAGE,
		(NihIoReader) sock_scm_reader,
		(NihIoCloseHandler) scm_sock_close,
		 scm_sock_error_handler, d)) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
			"Failed to queue scm message: %s", strerror(errno));
		return -1;
	}
	if (!kick_fd_client(sockfd))
		return -1;
	return 0;
}

int cgmanager_get_tasks (void *data, NihDBusMessage *message, const char *controller,
			const char *cgroup, int32_t **pids, size_t *nrpids)
{
	int fd = 0, ret;
	struct ucred ucred;
	socklen_t len;
	int32_t *tmp;

	if (message == NULL) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
			"message was null");
		return -1;
	}

	if (!dbus_connection_get_socket(message->connection, &fd)) {
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
					     "Could not get client socket.");
		return -1;
	}

	len = sizeof(struct ucred);
	NIH_MUST (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &ucred, &len) != -1);

	nih_info (_("GetTasks: Client fd is: %d (pid=%d, uid=%u, gid=%u)"),
			fd, ucred.pid, ucred.uid, ucred.gid);

	ret = get_tasks_main(message, controller, cgroup, ucred, &tmp);
	if (ret >= 0) {
		*nrpids = ret;
		*pids = tmp;
		ret = 0;
	} else
		nih_dbus_error_raise_printf (DBUS_ERROR_INVALID_ARGS,
					     "invalid request");
	return ret;
}

static dbus_bool_t allow_user(DBusConnection *connection, unsigned long uid, void *data)
{
	return TRUE;
}

static int
client_connect (DBusServer *server, DBusConnection *conn)
{
	if (server == NULL || conn == NULL)
		return FALSE;

	dbus_connection_set_unix_user_function(conn, allow_user, NULL, NULL);
	dbus_connection_set_allow_anonymous(conn, TRUE);

	nih_info (_("Connection from private client"));

	NIH_MUST (nih_dbus_object_new (NULL, conn,
				"/org/linuxcontainers/cgmanager",
				cgmanager_interfaces, NULL));

	return TRUE;
}

static void
client_disconnect (DBusConnection *conn)
{
	if (conn == NULL)
		return;

	nih_info (_("Disconnected from private client"));
}


/**
 * options:
 *
 * Command-line options accepted by this program.
 **/
static NihOption options[] = {
	{ 0, "daemon", N_("Detach and run in the background"),
		NULL, NULL, &daemonise, NULL },

	NIH_OPTION_LAST
};

static inline int mkdir_cgmanager_dir(void)
{
	if (mkdir(CGMANAGER_DIR, 0755) == -1 && errno != EEXIST) {
		nih_error("Could not create %s", CGMANAGER_DIR);
		return false;
	}
	return true;
}

static bool daemon_running(void)
{
	DBusConnection *server_conn;
	NihError *err;

	server_conn = nih_dbus_connect(CGMANAGER_DBUS_PATH, NULL);
	if (server_conn) {
		dbus_connection_unref (server_conn);
		return true;
	}
	err = nih_error_get();
	nih_free(err);
	return false;
}

/*
 * We may decide to make the socket path customizable.  For now
 * just assume it is in /sys/fs/cgroup/ which has some special
 * consequences
 */
static bool setup_cgroup_dir(void)
{
	int ret;
	if (!dir_exists(CGDIR)) {
		nih_debug(CGDIR " does not exist");
		return false;
	}
	if (daemon_running()) {
		nih_error("cgmanager is already running");
		return false;
	}
	if (file_exists(CGMANAGER_SOCK)) {
		if (unlink(CGMANAGER_SOCK) < 0) {
			nih_error("failed to delete stale cgmanager socket");
			return false;
		}
	}
	/* Check that /sys/fs/cgroup is writeable, else mount a tmpfs */
	unlink(CGPROBE);
	ret = creat(CGPROBE, O_RDWR);
	if (ret >= 0) {
		close(ret);
		unlink(CGPROBE);
		return mkdir_cgmanager_dir();
	}
	ret = mount("cgroup", CGDIR, "tmpfs", 0, "size=10000");
	if (ret) {
		nih_debug("Failed to mount tmpfs on %s: %s",
			CGDIR, strerror(errno));
		return false;
	}
	nih_debug("Mounted tmpfs onto %s", CGDIR);
	return mkdir_cgmanager_dir();
}

int
main (int argc, char *argv[])
{
	char **		args;
	int		ret;
	DBusServer *	server;
	struct stat sb;

	nih_main_init (argv[0]);

	nih_option_set_synopsis (_("Control group manager"));
	nih_option_set_help (_("The cgroup manager daemon"));

	args = nih_option_parser (NULL, argc, argv, options, FALSE);
	if (! args)
		exit (1);

	if (!setup_cgroup_dir()) {
		nih_fatal("Failed to set up cgmanager socket");
		exit(1);
	}

	/* Setup the DBus server */
	server = nih_dbus_server (CGMANAGER_DBUS_PATH, client_connect,
				  client_disconnect);
	nih_assert (server != NULL);

	if (setup_cgroup_mounts() < 0) {
		nih_fatal ("Failed to set up cgroup mounts");
		exit(1);
	}

	if (stat("/proc/self/ns/pid", &sb) == 0) {
		mypidns = read_pid_ns_link(getpid());
		setns_pid_supported = true;
	}

	if (stat("/proc/self/ns/user", &sb) == 0) {
		myuserns = read_user_ns_link(getpid());
		setns_user_supported = true;
	}

	/* Become daemon */
	if (daemonise) {
		if (nih_main_daemonise () < 0) {
			NihError *err;

			err = nih_error_get ();
			nih_fatal ("%s: %s", _("Unable to become daemon"),
					err->message);
			nih_free (err);

			exit (1);
		}
	}

	ret = nih_main_loop ();

	/* Destroy any PID file we may have created */
	if (daemonise) {
		nih_main_unlink_pidfile();
	}

	return ret;
}
