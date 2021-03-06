#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <time.h>

#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/epoll.h>
#include <sys/signal.h>
#include <arpa/inet.h>

#include <set>
#include <vector>
#include <algorithm>

#define DEFAULT_PORT 9134

#define MAX_DESC_PER_MESSAGE 256
#define MAX_CONTROL_MESSAGE_SIZE (4 + MAX_DESC_PER_MESSAGE * sizeof(int))
#define MAX_CONTROL_MESSAGE_CONTROL_SIZE (CMSG_SPACE(MAX_DESC_PER_MESSAGE * sizeof(int)))

template <int size, bool C>
struct optional_buf {
	char value[size];
	static const bool placeholder = false;
};
template <int size>
struct optional_buf<size, false> {
	char value[1];
	static const bool placeholder = true;
};

#define MAX_CONTROL_MESSAGE_TOTAL_SIZE (MAX_CONTROL_MESSAGE_SIZE + MAX_CONTROL_MESSAGE_CONTROL_SIZE)

#define likely(x) __builtin_expect((x),1)
#define unlikely(x) __builtin_expect((x),0)

struct fd_ctx;

int total_sockets = 0;

#define VPERROR(msg) vperror(msg, __FILE__, __LINE__)

int vperror(const char * msg, const char * srcfile = NULL, int srcline = -1) {
	if (srcfile) {
		fprintf(stderr, "%s:%d: %s: %s\n", srcfile, srcline, msg, strerror(errno));
	} else {
		fprintf(stderr, "%s: %s\n", msg, strerror(errno));
	}
}

#define MAX_PEERS 16
struct proxy_peers {
	int npeers;
	fd_ctx * peers[MAX_PEERS];
	void add(fd_ctx * p);
	fd_ctx * find(int uid);
	void remove(fd_ctx * p);
	void cleanup_dangling();
	void remove_from_all_peers(fd_ctx * p);
	void unref_all();
	proxy_peers() : npeers(0) { }
};

struct fd_ctx {
	int faf_uid;
	int fd;
	bool is_server;
	proxy_peers peers;
	int buf_len;
	int refcount;
	int protocol;
	char buf[1];

	void remove_myself_from_peer_caches() {
		peers.remove_from_all_peers(this);
	}
	void cache_remove(fd_ctx * p) {
		peers.remove(p);
	}
	fd_ctx() : refcount(1) { }
	~fd_ctx();
};

static const int FDCTX_CLIENT_BUFSIZE     = 4096 - sizeof(fd_ctx);
static const int FDCTX_TCP_SERVER_BUFSIZE = 256  - sizeof(fd_ctx);
static const int FDCTX_CTRL_BUFSIZE       = 0;

fd_ctx * allocate_fdctx(int bufsize) {
	void * memp = malloc(sizeof(fd_ctx) + bufsize);
	return new (memp) fd_ctx();
}

void deallocate_fdctx(fd_ctx * p) {
	p->~fd_ctx();
	free(p);
}

fd_ctx * proxy_peers::find(int uid) {
	for (int i = 0; i < npeers; ++i) {
		if (peers[i]->faf_uid == uid) return peers[i];
	}
	return NULL;
}

void proxy_peers::add(fd_ctx * p) {
	if (npeers < MAX_PEERS) {
		// insert fresh entries at the front
		fd_ctx * prev = peers[0];
		for (int i = 1; i < npeers; ++i) {
			fd_ctx * tmp = peers[i];
			peers[i] = prev;
			prev = tmp;
		}
		peers[npeers] = prev;
		peers[0] = p;
		++p->refcount;
		++npeers;
	} else {
		// remove oldest mapping
		if (--peers[npeers - 1]->refcount == 0) {
			deallocate_fdctx(peers[npeers - 1]);
		}
		--npeers;
		add(p);
	}
}

void proxy_peers::remove(fd_ctx * p) {
	for (int i = 0; i < npeers; ++i) {
		if (peers[i] == p) {
			++i;
			for (; i < npeers; ++i) {
				peers[i - 1] = peers[i];
			}
			--npeers;
			if (--p->refcount == 0) {
				deallocate_fdctx(p);
			}
			return;
		}
	}
}

void proxy_peers::remove_from_all_peers(fd_ctx * p) {
	for (int i = 0; i < npeers; ++i) {
		peers[i]->cache_remove(p);
	}
}

void proxy_peers::unref_all() {
	for (int i = 0; i < npeers; ++i) {
		if (--peers[i]->refcount == 0) {
			deallocate_fdctx(peers[i]);
		}
	}
	npeers = 0;
}

void proxy_peers::cleanup_dangling() {
	int oi = 0;
	for (int i = 0; i < npeers; ++i) {
		if (peers[i]->faf_uid == -1) {
			if (--peers[i]->refcount == 0) {
				deallocate_fdctx(peers[i]);
				continue;
			}
		}
		if (i != oi) peers[oi] = peers[i];
		++oi;
	}
	npeers = oi;
}

struct fd_ctx_less_by_uid {
	bool operator()(const fd_ctx * a, const fd_ctx * b) const {
		return a->faf_uid < b->faf_uid;
	}
};

struct proxy_msg_header {
	uint32_t size;
	uint16_t port;
	uint16_t destuid;
} __attribute__ ((packed));

struct proxy_msg_header_set_uid {
	uint32_t size;
	uint16_t uid;
} __attribute__ ((packed));

struct proxy_msg_header_to_peer {
	uint32_t size;
	uint16_t port;
} __attribute__ ((packed));

typedef std::set<fd_ctx *, fd_ctx_less_by_uid> peer_sockets_t;

#define OUT_HEADER_OFFSET_ADJ (sizeof(proxy_msg_header) - sizeof(proxy_msg_header_to_peer))

bool got_sigusr1 = false;

void sigusr1(int) {
	got_sigusr1 = true;
}

fd_ctx::~fd_ctx() {
	peers.unref_all();
}

char * get_ip_str(const struct sockaddr *sa, char *s, size_t maxlen) {
    switch(sa->sa_family) {
	case AF_INET:
		inet_ntop(AF_INET, &(((struct sockaddr_in *)sa)->sin_addr),
				  s, maxlen);
		break;
		
	case AF_INET6:
		inet_ntop(AF_INET6, &(((struct sockaddr_in6 *)sa)->sin6_addr),
				  s, maxlen);
		break;
		
	default:
		strncpy(s, "(unknown)", maxlen);
		return NULL;
    }
	
    return s;
}

int ctrl_socket_listen(int s, const char * path) {
	sockaddr_un sun;
	sun.sun_family = AF_UNIX;
	strncpy(sun.sun_path, path, sizeof(sun.sun_path));

	if (bind(s, (sockaddr *) &sun, sizeof(sun)) < 0) {
		fprintf(stderr, "bind(%s): %s\n", path, strerror(errno));
		return -1;
	}
	int on = 1;
	if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char *) &on, sizeof(on)) == -1) {
		VPERROR("setsockopt(REUSEADDR)");
		return -1;
	}
	if (listen(s, 1) < 0) {
		VPERROR("listen"); return -1;
	}
	return 0;
}

int poll_in(int epoll, fd_ctx * ptr) {
	epoll_event ev;
	ev.events   = EPOLLIN;
	ev.data.ptr = (void *) ptr;
	if (epoll_ctl(epoll, EPOLL_CTL_ADD, ptr->fd, &ev) < 0) {
		VPERROR("epoll_ctl"); return -1;
	}
	return 0;
}

template <typename Iter, typename Container>
int send_fds(int ctrlsock, int epoll, Iter beg, Iter end, Container * all) {
	char control[CMSG_SPACE(sizeof(int) * MAX_DESC_PER_MESSAGE)];
	char buf[4 + sizeof(int) * MAX_DESC_PER_MESSAGE];
	msghdr msg;

	msg.msg_name       = NULL;
	msg.msg_namelen    = 0;
	msg.msg_control    = control;
	msg.msg_controllen = sizeof(control);

	strcpy(buf, "desc");

	cmsghdr * cmp = CMSG_FIRSTHDR(&msg);

	cmp->cmsg_level = SOL_SOCKET;
	cmp->cmsg_type  = SCM_RIGHTS;

	int fd_count = 0;
	Iter erase_beg;
	bool erase_valid = false;
	std::vector<int> to_close;

	for (int * uidp = (int *) (buf + 4);
		 beg != end && fd_count < MAX_DESC_PER_MESSAGE;
		 ++beg, ++uidp)
	    {
			if ((**beg).buf_len == 0) {
				if (! erase_valid) {
					erase_beg = beg;
				    erase_valid = true;
				}
				*uidp = (**beg).faf_uid;

				* ((int *) CMSG_DATA(cmp) + fd_count) = (**beg).fd;
				to_close.push_back((**beg).fd);
				//if (epoll_ctl(epoll, EPOLL_CTL_DEL, (**beg).fd, NULL) < 0) {
				//					VPERROR("epoll_ctl(DEL)");
				//				}
				++fd_count;
			} else {
				if (erase_valid) {
					if (all) all->erase(erase_beg, beg);
					erase_valid = false;
				}
			}
		}

	cmp->cmsg_len   = CMSG_LEN(sizeof(int) * fd_count);
	if (erase_valid) {
		if (all) all->erase(erase_beg, beg);
	}
	msg.msg_controllen  = CMSG_SPACE(fd_count * sizeof(int));
	iovec iov;
	iov.iov_base   = buf;
	iov.iov_len    = 4 + fd_count * sizeof(int);
	msg.msg_iov    = &iov;
	msg.msg_iovlen = 1;

	if (fd_count) {
		if (sendmsg(ctrlsock, &msg, 0) < 0) {
			VPERROR("sendmsg");
		} else {
			total_sockets -= to_close.size();
			// we dont care about caches and refcounts and destroying contexts,
			// so we cheat and handle the global counters here
			for (int i = 0; i < to_close.size(); ++i) {
				if (epoll_ctl(epoll, EPOLL_CTL_DEL, to_close[i], NULL) < 0) {
					VPERROR("epoll_ctl");
				}
				
				close(to_close[i]);
			}
		}
	}
	return fd_count;
}

template <typename T>
struct dummy_erase_container {
	void erase(T *, T *) { }
};

int send_fd(int ctrlsock, int epoll, fd_ctx * ctxp) {
	return send_fds(ctrlsock, epoll, &ctxp, &ctxp + 1, (dummy_erase_container<fd_ctx *> *) NULL);
}

int main(int argc, char ** argv) {
	int listen_port = -1;
	char listen_port_str[8];
	const char * ctrl_socket_path = NULL;

	{
		int opt;
		while ((opt = getopt(argc, argv, "p:hu:")) != EOF) {
			switch (opt) {
			case 'p' :
				listen_port = atoi(optarg);
				break;
			case 'h' :
				fprintf(stderr, "%s [-p port] [-u socket-path]\n", argv[0]);
				fprintf(stderr, "default: -p 9134\n");
				exit(0);
			case 'u' :
				ctrl_socket_path = optarg;
				break;
			}
		}
		argc -= optind;
		argv += optind;
	}

	if (listen_port == -1) {
		listen_port = 9134;
	}
	sprintf(listen_port_str, "%d", listen_port);

	typedef std::vector<fd_ctx *> server_sockets_t;
	server_sockets_t server_sockets;
	peer_sockets_t peer_sockets;

	fd_ctx ctrl_socket, ctrl_socket_conn;
	bool ctrl_socket_mode_listen = false;
	bool decay_mode = false;

	ctrl_socket.fd = -1;
	ctrl_socket_conn.fd = -1;

	int sockets_inherited = 0;

	int epoll = epoll_create(1024);
	if (epoll < 0) {
		VPERROR("epoll_create"); exit(1);
	}

	if (ctrl_socket_path) {
		int s = socket(PF_UNIX, SOCK_SEQPACKET, 0);
		if (s < 0) {
			VPERROR("socket(AF_UNIX)");
			exit(1);
		}
		struct sockaddr_un sun;
		sun.sun_family = AF_UNIX;
		strncpy(sun.sun_path, ctrl_socket_path, sizeof(sun.sun_path));

		if (connect(s, (sockaddr *) &sun, sizeof(sun))) {
			if (errno == ECONNREFUSED || errno == ENOENT) {
				if (errno == ECONNREFUSED) {
					if (unlink(ctrl_socket_path) < 0) {
						fprintf(stderr, "unlink(%s): %s\n", ctrl_socket_path, strerror(errno));
						exit(1);
					}
				}
				ctrl_socket_listen(s, ctrl_socket_path);
				ctrl_socket.fd = s;
				poll_in(epoll, &ctrl_socket);
				ctrl_socket_mode_listen = true;
			} else {
				fprintf(stderr, "connect(%s): %s\n", ctrl_socket_path, strerror(errno));
			}
		} else {
			char buf[16];
			ssize_t n = send(s, "unlisten", sizeof("unlisten") - 1, 0);
			if (n < 0) {
				VPERROR("sendmsg");
				exit(1);
			} else if (n == 0) {
				fprintf(stderr, "unexpected EOF\n");
				exit(1);
			}

			// blocking read
			n = recv(s, buf, sizeof(buf), 0);
			if (strncmp(buf, "unlistening", strlen("unlistening")) != 0) {
				fprintf(stderr, "running server reported: ");
				fwrite(buf, n, 1, stderr);
				exit(1);
			}
			ctrl_socket_conn.fd = s;
			poll_in(epoll, &ctrl_socket_conn);
		}
	}

	{
		struct addrinfo hints, * ai_res;
		hints.ai_family   = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;
		hints.ai_flags    = AI_PASSIVE;

		int r = getaddrinfo(NULL, listen_port_str, &hints, &ai_res);
		if (r) {
			fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(r));
			exit(1);
		}

		for (struct addrinfo * ai = ai_res; ai; ai = ai->ai_next) {
			int s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
			if (s < 0) {
				VPERROR("socket"); exit(1);
			}
			if (ai->ai_family == AF_INET6) {
				int on = 1;
				if (setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, 
							   (char *)&on, sizeof(on)) == -1) {
					VPERROR("setsockopt(IPV6_ONLY)");
					exit(1);
				}
			}
			{
				int on = 1;
				if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char *) &on, sizeof(on)) == -1) {
					VPERROR("setsockopt(REUSEADDR)");
					exit(1);
				}
			}
			if (bind(s, ai->ai_addr, ai->ai_addrlen) < 0) {
				VPERROR("bind"); exit(1);
			}
			if (listen(s, 50) < 0) {
				VPERROR("listen"); exit(1);
			}
			fd_ctx * c = allocate_fdctx(FDCTX_TCP_SERVER_BUFSIZE);
			c->fd = s;
			c->is_server = true;
			c->protocol  = ai->ai_protocol;
			char * strp  = c->buf;
			int slen     = FDCTX_TCP_SERVER_BUFSIZE;
			if (ai->ai_family == AF_INET6) {
				*strp++ = '[';
				slen -= 2;
			}
			get_ip_str(ai->ai_addr, strp, slen);
			if (ai->ai_family == AF_INET6) {
				strcat(c->buf, "]");
			}
			sprintf(c->buf + strlen(c->buf), ":%d", listen_port);
			server_sockets.push_back(c);
		}
		freeaddrinfo(ai_res);
	}

	for (int i = 0; i < server_sockets.size(); ++i) {
		poll_in(epoll, server_sockets[i]);
	}

	epoll_event epoll_events[32];
	const int epoll_max_events = 32;

	fd_ctx fd_ctx_finder;
	signal(SIGUSR1, sigusr1);
	signal(SIGPIPE, SIG_IGN);

	total_sockets  = server_sockets.size();
	time_t status_time = time(NULL);

	while (total_sockets) {
		if (unlikely(got_sigusr1)) {
			// close listening sockets
			for (int i = 0; i < server_sockets.size(); ++i) {
				fprintf(stderr, "close server %s\n", server_sockets[i]->buf);
				if (epoll_ctl(epoll, EPOLL_CTL_DEL, server_sockets[i]->fd, NULL) < 0) {
					VPERROR("epoll_ctl");
				}
				close(server_sockets[i]->fd);
				--total_sockets;
			}
			got_sigusr1 = false;
		}
		if (unlikely(status_time + 5 < time(NULL))) {
			fprintf(stderr, "%d connections, %d identified peers\n", total_sockets - server_sockets.size(), peer_sockets.size());
			status_time = time(NULL);
		}

		int ep_num = epoll_wait(epoll, epoll_events, epoll_max_events, 1000);
		if (unlikely(ep_num < 0)) {
			if (errno == EINTR) continue;
			VPERROR("epoll_wait"); continue;
		}
		bool epoll_restart = false;
		for (int epi = 0; epi < ep_num && ! epoll_restart; ++epi) {
			fd_ctx * ctxp = (fd_ctx *) epoll_events[epi].data.ptr;

			if (unlikely(ctxp == &ctrl_socket)) {
				sockaddr_storage ss;
				socklen_t sl = sizeof(ss);
				
				int nsock = accept(ctxp->fd, (sockaddr *) &ss, &sl);
				if (nsock < 0) {
					VPERROR("accept"); continue;
				}
				epoll_event ev;
				ev.events   = EPOLLIN;
				ev.data.ptr = (void *) &ctrl_socket_conn;
				if (epoll_ctl(epoll, EPOLL_CTL_ADD, nsock, &ev) < 0) {
					VPERROR("epoll_ctl");
					close(nsock);
					continue;
				}

				// we only ever accept one ctrl client at a time
				if (epoll_ctl(epoll, EPOLL_CTL_DEL, ctrl_socket.fd, NULL) < 0) {
					VPERROR("epoll_ctl");
					close(nsock);
					continue;
				}
				ctrl_socket_conn.fd = nsock;
			} else if (unlikely(ctxp == &ctrl_socket_conn)) {
				if (ctrl_socket_mode_listen) {
					char buf[32];

					int n = read(ctxp->fd, buf, sizeof(buf));
					if (n < 0) {
						if (errno == EINTR || errno == EAGAIN) continue;
						VPERROR("read");
						close(ctxp->fd);
						poll_in(epoll, &ctrl_socket);
					} else if (n == 0) {
						close(ctxp->fd);
						poll_in(epoll, &ctrl_socket);
					} else {
						if (strncmp(buf, "unlisten", sizeof("unlisten") - 1) == 0) {
							for (int i = 0; i < server_sockets.size(); ++i) {
								fprintf(stderr, "close server %s\n", server_sockets[i]->buf);
								if (epoll_ctl(epoll, EPOLL_CTL_DEL, server_sockets[i]->fd, NULL) < 0) {
									VPERROR("epoll_ctl");
								}
								close(server_sockets[i]->fd);
								--total_sockets;
							}
							if (write(ctrl_socket_conn.fd, "unlistening", sizeof("unlistening") - 1) < 0) {
								VPERROR("write");
							} else {
								int nsent = 0;
								
								do {
									nsent = send_fds(ctrl_socket_conn.fd, epoll, peer_sockets.begin(), peer_sockets.end(), &peer_sockets);
									if (nsent) {
										fprintf(stderr, "bulk send: %d\n", nsent);
									}
								} while (nsent && ! peer_sockets.empty());
								epoll_restart = true;
								decay_mode = true;
							}
						}
					}
				} else {
					msghdr msg;
					iovec iov;
					char control[MAX_CONTROL_MESSAGE_CONTROL_SIZE];
					char * controlp = control;
					//					optional_buf<MAX_CONTROL_MESSAGE_CONTROL_SIZE, (MAX_CONTROL_MESSAGE_TOTAL_SIZE > FDCTX_BUFFER_SIZE)> control;
					//					char * controlp = control.placeholder ?
					//						ctxp->buf + MAX_CONTROL_MESSAGE_SIZE :
					//						control.value;
					//					optional_buf<MAX_CONTROL_MESSAGE_SIZE, (MAX_CONTROL_MESSAGE_SIZE > FDCTX_BUFFER_SIZE)> buf;
					//					char * bufp = buf.placeholder ?	ctxp->buf : control.value;
					char buf[MAX_CONTROL_MESSAGE_SIZE];
					char * bufp = buf;

					iov.iov_base = bufp;
					iov.iov_len  = MAX_CONTROL_MESSAGE_SIZE;

					msg.msg_name       = NULL;
					msg.msg_namelen    = 0;
					msg.msg_iov        = &iov;
					msg.msg_iovlen     = 1;
					msg.msg_control    = (void *) controlp;
					msg.msg_controllen = MAX_CONTROL_MESSAGE_CONTROL_SIZE;
					msg.msg_flags      = 0;

					int n = recvmsg(ctxp->fd, &msg, 0);
					if (n < 0) {
						VPERROR("recvmsg");
					} else if (n == 0) {
						fprintf(stderr, "unexpected close\n");
						close(ctxp->fd);
					} else {
						if (strncmp((const char *) iov.iov_base, "desc", std::min(4, n)) == 0) {
							cmsghdr * cmp = CMSG_FIRSTHDR(&msg);
							if (cmp->cmsg_level != SOL_SOCKET || cmp->cmsg_type != SCM_RIGHTS) {
								fprintf(stderr, "malformed control message: wrong type\n");
								exit(1);
							}

							int * uidp = (int *) ((char *) iov.iov_base + 4);
							int * uidpend = (int *) ((char *) iov.iov_base + n);

							int fd_count = 0;
							for (; uidp < uidpend; ++uidp, ++fd_count) {
								int fd = * ((int *) CMSG_DATA(cmp) + fd_count);
								++sockets_inherited;
								++total_sockets;
								fd_ctx * cp = allocate_fdctx(FDCTX_CLIENT_BUFSIZE);
								cp->fd = fd;
								cp->faf_uid = *uidp;
								cp->is_server = false;
								cp->protocol = IPPROTO_TCP;
								cp->buf_len = 0;
								epoll_event ev;
								ev.events = EPOLLIN;
								ev.data.ptr = (void *) cp;
								if (epoll_ctl(epoll, EPOLL_CTL_ADD, cp->fd, &ev) < 0) {
									VPERROR("epoll_ctl");
									--total_sockets;
									close(cp->fd);
									deallocate_fdctx(cp);
								}
								if (cp->faf_uid != -1) {
									peer_sockets.insert(cp);
								}
							}
						} else if (strncmp((const char *) iov.iov_base, "exit", std::min(4, n)) == 0) {
							close(ctxp->fd);
							int s = socket(PF_UNIX, SOCK_SEQPACKET, 0);
							if (s < 0) {
								VPERROR("socket(PF_UNIX)");
							} else {
								ctrl_socket_listen(s, ctrl_socket_path);
								ctrl_socket.fd = s;
								poll_in(epoll, &ctrl_socket);
								ctrl_socket_mode_listen = true;
							}
							fprintf(stderr, "%d sockets inherited from the dead\n", sockets_inherited);
						}
					}
				}
			} else if (unlikely(ctxp->is_server && ctxp->protocol == IPPROTO_TCP)) {
				sockaddr_storage saddr;
				socklen_t saddrlen = sizeof(saddr);
				int nsock = accept(ctxp->fd, (sockaddr *) &saddr, &saddrlen);
				if (nsock < 0) {
					VPERROR("accept");
				} else {
					++total_sockets;
					fd_ctx * cp = allocate_fdctx(FDCTX_CLIENT_BUFSIZE);
					cp->fd = nsock;
					cp->faf_uid = -1;
					cp->is_server = false;
					cp->protocol = IPPROTO_TCP;
					cp->buf_len = 0;

					epoll_event ev;
					ev.events = EPOLLIN;
					ev.data.ptr = (void *) cp;
					if (epoll_ctl(epoll, EPOLL_CTL_ADD, nsock, &ev) < 0) {
						VPERROR("epoll_ctl");
						--total_sockets;
						close(nsock);
						deallocate_fdctx(cp);
					}
				}
			} else {
				if (unlikely(decay_mode && ctxp->buf_len == 0)) {
					fprintf(stderr, "single send\n");
					send_fd(ctrl_socket_conn.fd, epoll, ctxp);
					if (ctxp->faf_uid != -1) {
						peer_sockets.erase(ctxp);
					}
					continue; // -> next epoll result
				}

				int n = read(ctxp->fd, ctxp->buf + ctxp->buf_len, FDCTX_CLIENT_BUFSIZE - ctxp->buf_len);
				if (unlikely(n < 0)) {
					if (errno != ECONNRESET && errno != EAGAIN && errno != EINTR) {
						VPERROR("read");
					}
					continue;
				} else if (unlikely(n == 0)) {
					close(ctxp->fd);
					--total_sockets;
					if (ctxp->faf_uid != -1) {
						peer_sockets.erase(ctxp);
					}
					ctxp->remove_myself_from_peer_caches();
					--ctxp->refcount;
					if (ctxp->refcount == 0) {
						deallocate_fdctx(ctxp);
					} else {
						ctxp->faf_uid = -1;
					}
				} else {
					ctxp->buf_len += n;
					char * buf_head = ctxp->buf;
					bool postprocess = true;

					while (buf_head < ctxp->buf + ctxp->buf_len) {
						const int buf_len = ctxp->buf + ctxp->buf_len - buf_head;
						if (buf_len < 4) {
							break;
						}
						proxy_msg_header * h = (proxy_msg_header *) buf_head;
						const int in_msg_size = ntohl(h->size);

						if (unlikely(in_msg_size > FDCTX_CLIENT_BUFSIZE)) {
							// message to big
							if (epoll_ctl(epoll, EPOLL_CTL_DEL, ctxp->fd, NULL) < 0) {
								VPERROR("epoll_ctl");
							}
							close(ctxp->fd);
							--total_sockets;
							if (ctxp->faf_uid != -1) {
								peer_sockets.erase(ctxp);
							}
							ctxp->remove_myself_from_peer_caches();
							--ctxp->refcount;
							if (ctxp->refcount == 0) {
								deallocate_fdctx(ctxp);
							} else {
								ctxp->faf_uid = -1;
							}
							postprocess = false;
							break;
						}

						if (in_msg_size + 4 > buf_len) {
							break;
						}

						if (unlikely(ctxp->faf_uid == -1)) {
							proxy_msg_header_set_uid * hu = (proxy_msg_header_set_uid *) h;
							ctxp->faf_uid = ntohs(hu->uid);
							peer_sockets.insert(ctxp);

							buf_head += in_msg_size + 4;
							continue; // -> next message from this fd_ctx
						}

						// in decay mode we always drop, because we expect our
						// caches and refcounts to be inconsistent
						// we can decay without bookkeeping if we never send any packets
						// out (== we never expect a context to exists unless epoll still
						// knows about it)
						if (! decay_mode) {
							int uid = ntohs(h->destuid);

							fd_ctx * peer = ctxp->peers.find(uid);

							if (unlikely(! peer)) {
								fd_ctx_finder.faf_uid = uid;
								peer_sockets_t::iterator iter = peer_sockets.find(&fd_ctx_finder);
								if (iter != peer_sockets.end()) {
									peer = *iter;
									ctxp->peers.add(peer);
								} else {
									buf_head += in_msg_size + 4;
									continue;
								}
							}
							
							int in_port = ntohs(h->port);
							proxy_msg_header_to_peer * hout = (proxy_msg_header_to_peer *) (buf_head + OUT_HEADER_OFFSET_ADJ);
							hout->port = htons(in_port);
							const int out_size = in_msg_size - OUT_HEADER_OFFSET_ADJ;
							hout->size = htonl(out_size);
							
							{
								int n = write(peer->fd, (char *) hout, out_size + 4);
								if (unlikely(n < 0)) {
									if (errno != ECONNRESET && errno != EPIPE) {
										VPERROR("write");
									}
								} else if (unlikely(n != out_size + 4)) {
									fprintf(stderr, "short write (%d of %d\n", n, out_size + 4);
								}
							}
						}
						buf_head += in_msg_size + 4;
					}
					if (likely(postprocess)) {
						int new_buflen = ctxp->buf + ctxp->buf_len - buf_head;

						if (unlikely(new_buflen && ctxp->buf != buf_head)) {
							for (char * p = ctxp->buf; buf_head < ctxp->buf + ctxp->buf_len; ++p, ++buf_head) {
								*p = *buf_head;
							}
						}
						ctxp->buf_len = new_buflen;
					}
					// we want to get rid of clients as soon as possible and
					// dont wait for them to send the next message to trigger it
					if (unlikely(decay_mode && ctxp->buf_len == 0)) {
						send_fd(ctrl_socket_conn.fd, epoll, ctxp);
						if (ctxp->faf_uid != -1) {
							peer_sockets.erase(ctxp);
						}
					}
				}
			}
		}
	}
	if (decay_mode && ctrl_socket_path) {
		close(ctrl_socket.fd);
		unlink(ctrl_socket_path);
		if (write(ctrl_socket_conn.fd, "exit", strlen("exit")) < 0) {
			VPERROR("send");
		}
	}
	fprintf(stderr, "exit due to %d sockets left to serve\n", total_sockets);
	exit(0);
}
