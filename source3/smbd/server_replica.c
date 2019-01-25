#include "apue.h"
#include <sys/socket.h>
#include <sys/user.h>
#include <libutil.h>

#define CMSG_LEN(x) _CMSG_DATA_ALIGN(sizeof(struct cmsghdr)+(x))
/* size of control buffer to send/recv one file descriptor */
#define CONTROLLEN  CMSG_LEN(sizeof(int))

static int ncpus;

static int smbd_create_replicas(void);

static struct replica_info {
	int ri_pid;
	int ri_fd;
	struct cmsghdr *ri_cmptr;
} *replicas;


/*
 * Pass a file descriptor to another process.
 * If fd<0, then -fd is sent back instead as the error status.
 */
static int
send_fd(int fd, struct cmsghdr *cmptr, int fd_to_send)
{
    struct iovec    iov[1];
    struct msghdr   msg;
    char            buf[2]; /* send_fd()/recv_fd() 2-byte protocol */

    iov[0].iov_base = buf;
    iov[0].iov_len  = 2;
    msg.msg_iov     = iov;
    msg.msg_iovlen  = 1;
    msg.msg_name    = NULL;
    msg.msg_namelen = 0;
    if (fd_to_send < 0) {
        msg.msg_control    = NULL;
        msg.msg_controllen = 0;
        buf[1] = -fd_to_send;   /* nonzero status means error */
        if (buf[1] == 0)
            buf[1] = 1; /* -256, etc. would screw up protocol */
    } else {
        cmptr->cmsg_level  = SOL_SOCKET;
        cmptr->cmsg_type   = SCM_RIGHTS;
        cmptr->cmsg_len    = CONTROLLEN;
        msg.msg_control    = cmptr;
        msg.msg_controllen = CONTROLLEN;
        *(int *)CMSG_DATA(cmptr) = fd_to_send;     /* the fd to pass */
        buf[1] = 0;          /* zero status means OK */
    }
    buf[0] = 0;              /* null byte flag to recv_fd() */
    if (sendmsg(fd, &msg, 0) != 2)
        return(-1);
    return(0);
}

/*
 * Receive a file descriptor from a server process.  Also, any data
 * received is passed to (*userfunc)(STDERR_FILENO, buf, nbytes).
 * We have a 2-byte protocol for receiving the fd from send_fd().
 */
static int
recv_fd(int fd, struct cmsghdr *cmptr)
{
   int             newfd, nr, status;
   char            *ptr;
   char            buf[MAXLINE];
   struct iovec    iov[1];
   struct msghdr   msg;

   status = -1;
   for ( ; ; ) {
       iov[0].iov_base = buf;
       iov[0].iov_len  = sizeof(buf);
       msg.msg_iov     = iov;
       msg.msg_iovlen  = 1;
       msg.msg_name    = NULL;
       msg.msg_namelen = 0;
       msg.msg_control    = cmptr;
       msg.msg_controllen = CONTROLLEN;
       if ((nr = recvmsg(fd, &msg, 0)) < 0) {
           err_sys("recvmsg error");
       } else if (nr == 0) {
           err_ret("connection closed by server");
           return(-1);
       }
       /*
        * See if this is the final data with null & status.  Null
        * is next to last byte of buffer; status byte is last byte.
        * Zero status means there is a file descriptor to receive.
        */
       for (ptr = buf; ptr < &buf[nr]; ) {
           if (*ptr++ == 0) {
               if (ptr != &buf[nr-1])
                   err_dump("message format error");
               status = *ptr & 0xFF;  /* prevent sign extension */
               if (status == 0) {
                   if (msg.msg_controllen != CONTROLLEN)
                       err_dump("status = 0 but no fd");
                   newfd = *(int *)CMSG_DATA(cmptr);
               } else {
                   newfd = -status;
               }
               nr -= 2;
           }
        }
	   if (nr > 0)
		   return(-1);
	   if (status >= 0)    /* final data has arrived */
		   return(newfd);  /* descriptor, or -status */
   }
}

struct replica_state {
	struct messaging_context *rs_msg_ctx;
	struct tevent_context *rs_ev;

};
static int last_replica;

static int
smbd_replica_fork_request(struct messaging_context *msg_ctx,
						  struct tevent_context *ev,
						  int fd_to_send)
{
	struct replica_state rs;
	struct replica_info *ri;
	pid_t pid;

	rs.rs_msg_ctx = msg_ctx;
	rs.rs_ev = ev;
	if (smbd_create_replicas(&rs))
		return (-1);

	ri = &replicas[last_replica];
	/* pass new file descriptor to replica */
	send_fd(ri->ri_fd, ri->ri_cmptr, fd_to_send);
	/* read back child pid */
	rc = read(ri->ri_fd, &pid, sizeof(pid));
	if (++last_replica == ncpus)
		last_replica = 0;
	if (rc == -1)
		return (-1);
	return (pid);
}
	
static int
smbd_replica_fork_handler(struct replica_state *rs, int fd)
{
	/*
	 * demarshal data
	 */
	
	return (smbd_replica_do_fork(rs->rs_msg_ctx, rs->rs_ev, fd));
	
}
							 
static int
smbd_replica_do_fork(struct messaging_context *msg_ctx,
					 struct tevent_context *ev,
					 int fd)
{
	int pid;
	
	pid = fork();
	if (pid == 0) {
		NTSTATUS status = NT_STATUS_OK;

		/* Stop zombies, the parent explicitly handles
		 * them, counting worker smbds. */
		CatchChild();

		status = smbd_reinit_after_fork(msg_ctx, ev, true, NULL);
		if (!NT_STATUS_IS_OK(status)) {
			if (NT_STATUS_EQUAL(status,
					    NT_STATUS_TOO_MANY_OPENED_FILES)) {
				DEBUG(0,("child process cannot initialize "
					 "because too many files are open\n"));
				goto exit;
			}
			if (lp_clustering() &&
			    (NT_STATUS_EQUAL(
				    status, NT_STATUS_INTERNAL_DB_ERROR) ||
			     NT_STATUS_EQUAL(
				    status, NT_STATUS_CONNECTION_REFUSED))) {
				DEBUG(1, ("child process cannot initialize "
					  "because connection to CTDB "
					  "has failed: %s\n",
					  nt_errstr(status)));
				goto exit;
			}

			DEBUG(0,("reinit_after_fork() failed\n"));
			smb_panic("reinit_after_fork() failed");
		}

		smbd_process(ev, msg_ctx, fd, false);
	 exit:
		exit_server_cleanly("end of child");
		return;
	}

	if (pid < 0) {
		DEBUG(0,("smbd_accept_connection: fork() failed: %s\n",
			 strerror(errno)));
	}

	/* The parent doesn't need this socket */
	close(fd);
	return pid;
}

static int
get_ncpus(void)
{
	size_t sizeof_ncpus;

	if (ncpus != 0)
		return (ncpus);
	sizeof_ncpus = sizeof(ncpus);
	if (sysctlbyname("hw.ncpu", &ncpus, &sizeof_ncpus,
	    (void *)NULL, 0) == -1)
		return (-1);

	return (ncpus);
}

#define STARTDATA "REPLICA START"

static void
smbd_replica_loop(struct replica_state *rs, int fd)
{
	int newfd, rc;
	struct cmsghdr *cmptr;
	pid_t pid;

	rc = write(ri->ri_fd, STARTDATA, sizeof(STARTDATA));
	cmptr = malloc(CONTROLLEN);
	if (cmptr == NULL)
		smb_panic("failed to allocate control message header\n");

	for ( ; ; ) {
		newfd = recv_fd(fd, cmptr);
		if (newfd == -1)
			smb_panic("recv_fd failed\n");
		pid = smbd_replica_fork_handler(rs, newfd);
		if (pid == -1)
			smb_panic("smbd_replica_fork_hander failed\n");
		write(ri->ri_fd, pid sizeof(pid));
	}
}

static int
privatize_mappings(void)
{
	struct kinfo_vmentry *kve;
	uint8_t *kve_start;
	uint8_t *va_start, *va_end;
	uint8_t data;
	int i, count;

	kve = kinfo_getvmmap(getpid(), &count);
	if (kve == NULL)
		return (-1);
	if (kve->kve_structsize != sizeof(*kve))
		DEBUG(0, ("kinfo_vmentry size mismatch - likely ABI breakage, recompile\n"));

	kve_start = (uint8_t *)kve;
	for (i = 0; i < count; i++) {
		/* cope with ABI breakage if changes added to the end */
		kve = (void *)(kve_start + i*kve->kve_structsize);
		if ((kve->kve_protection & KVME_PROT_WRITE) == 0)
			continue;
		va_start = kve->kve_start;
		va_end = kve->kve_end;
		while (va_start < va_end) {
			/*
			 * NB: This needs to use cmpxchg if
			 * this is multi-threaded
			 */
			data = *va_start;
			/* force CoW fault */
			*va_start = data;
			va_start += PAGE_SIZE;
		}
	}
	free(kve);
	return (0);
}

static int
smbd_replica_child(struct replica_state *rs, int fd)
{

	if (privatize_mappings())
		return (-1);
	
	/* handle fork requests */
	smbd_replica_loop(rs, fd);
}
	
static int
smbd_create_replica(struct replica_state *rs, struct replica_info *ri)
{
	int sockets[2], rc;
	char buf[64];
	struct cmsghdr *cmptr;
	pid_t pid;

	cmptr = malloc(CONTROLLLEN);
	if (cmptr == NULL)
		smb_panic("failed to allocate control message header\n");
	rc = socketpair(AF_UNIX, SOCK_STREAM, 0, sockets);
	if (rc == -1) {
		free(cmptr);
		return (rc);
	}
	pid = fork();
	if (pid == -1) {
		free(cmptr);
		close(sockets[0]);
		close(sockets[1]);
		return (-1);
	}
	if (pid == 0) {
		close(sockets[0]);
		if (smbd_replica_child(rs, sockets[1]))
			return (-1);
	} else {
		close(sockets[1]);
		ri->ri_pid = pid;
		ri->ri_fd = sockets[0];
		ri->ri_cmptr = cmptr;
		rc = read(ri->ri_fd, buf, sizeof(buf));
		if (rc == -1 || strcmp(buf != STARTDATA))
			return (-1);
	}
	return (0);
}

static int
smbd_create_replicas(struct replica_state *rs)
{
	int i, count;

	count = get_ncpus();
	if (num_replicas == count)
		return (0);

	replicas = malloc(sizeof(*replicas)*count);
	if (replicas == NULL)
		return (-1);
	for (i = 0; i < count; i++) {
		if (smbd_create_replica(rs, &replicas[i])) {
			/* XXX - shutdown replicas */
			return (-1);
		}
	}
}
