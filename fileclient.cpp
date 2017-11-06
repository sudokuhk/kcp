/*
 * @Author: kun huang
 * @Email:  sudoku.huang@gmail.com
 * @Desc:   reference google-tcmalloc,
 *          memory allocator and manager.
 *
 * Created by huangkun on 11/02/17.
 */

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <unistd.h>
#include <netdb.h>
#include <string.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/types.h>

#include "ikcp.h"

/* get system time */
static inline void itimeofday(long *sec, long *usec)
{
    struct timeval time;
    gettimeofday(&time, NULL);
    if (sec) *sec = time.tv_sec;
    if (usec) *usec = time.tv_usec;
}

/* get clock in millisecond 64 */
static inline IINT64 iclock64(void)
{
    long s, u;
    IINT64 value;
    itimeofday(&s, &u);
    value = ((IINT64)s) * 1000 + (u / 1000);
    return value;
}

static inline IUINT32 iclock()
{
    return (IUINT32)(iclock64() & 0xfffffffful);
}

int set_nonblock(int sock)
{
    int32_t tmp = fcntl(sock, F_GETFL, 0);
    tmp |= O_NONBLOCK;
    
    return fcntl(sock, F_SETFL, tmp);
}

typedef struct user_data
{
    int sock;
    struct sockaddr * peer;
    socklen_t addrlen;
} user_data;

static int on_send_udp(const char * data, int size, ikcpcb * kcp, void * user)
{
    user_data * sockinfo = static_cast<user_data *>(user);
    
    return sendto(sockinfo->sock, data, size, 0,
                  sockinfo->peer, sockinfo->addrlen);
}

int main(int argc, char * argv[])
{
    if (argc < 5) {
        fprintf(stderr, "less arguments:%d, need 5\n", argc);
        fprintf(stderr, "%s ip port remotefilename filename\n", argv[0]);
        exit(0);
    }
    
    struct addrinfo hints, *local, *peer;
    memset(&hints, 0, sizeof(struct addrinfo));
    
    hints.ai_flags      = AI_PASSIVE;
    hints.ai_family     = AF_INET;
    hints.ai_socktype   = SOCK_STREAM;
    hints.ai_socktype   = SOCK_DGRAM;
    
    if (0 != getaddrinfo(NULL, "1234", &hints, &local)) {
        fprintf(stderr, "incorrect network address.\n");
        return 0;
    }

    int sock = socket(local->ai_family, local->ai_socktype, local->ai_protocol);
    freeaddrinfo(local);
    
    if (0 != getaddrinfo(argv[1], argv[2], &hints, &peer)) {
        fprintf(stderr, "incorrect server/peer address. %s:%s\n", argv[1], argv[2]);
        return 0;
    }
    
    user_data * user = new user_data;
    user->sock = sock;
    user->peer = peer->ai_addr;
    user->addrlen = peer->ai_addrlen;
    
    set_nonblock(sock);
    
    ikcpcb * kcp = ikcp_create(1, user);
    ikcp_nodelay(kcp, 1, 10, 2, 1);
    ikcp_setminrto(kcp, 10);
    ikcp_setoutput(kcp, on_send_udp);
    ikcp_wndsize(kcp, 1024, 1024);
    
    ikcp_send(kcp, argv[3], (int)strlen(argv[3]));
    ikcp_update(kcp, iclock());
    
    char * buf = new char[2048];
    int fsize = 0;
    int maxfd = sock + 1;
    struct timeval tv = {0, 10000};   //100ms
    int nread = 0, ntotal = 0;;
    int npeek = 0;
    IUINT32 lastrecv = iclock();
    bool brecv = false;
    
    int ifd = open(argv[4], O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (ifd < 0) {
        fprintf(stderr, "create file:%s error:%s\n", argv[4], strerror(errno));
        exit(0);
    }
    
    while (true) {
        fd_set rdset;
        FD_ZERO(&rdset);
        FD_SET(sock, &rdset);
        brecv = false;
        
        int ret = select(maxfd, &rdset, NULL, NULL, &tv);
        if (ret < 0) {
            fprintf(stderr, "select error:%s\n", strerror(errno));
            break;
        } else if (ret > 0) {
            if (FD_ISSET(sock, &rdset)) {
                int count = 10;
                while (count -- > 0 && (nread = read(sock, buf, 2048)) > 0) {
                    //fprintf(stdout, "recv :%d\n", nread);
                    ikcp_input(kcp, buf, nread);
                    brecv = true;
                }
            }
        }
        
        int now = iclock();
        ikcp_update(kcp, now);
        
        int count = 10;
        while (count -- > 0 && (npeek = ikcp_peeksize(kcp)) > 0) {
            npeek = ikcp_recv(kcp, buf, npeek);
            //fprintf(stdout, "ikcp recv:%d\n", npeek);
            if (npeek <= 0) {
                break;
            }
            
            if (fsize == 0) {
                fsize = *(int *)buf;
                fsize = ntohl(fsize);
                fprintf(stdout, "fsize:%d\n", fsize);
            } else {
                write(ifd, buf, npeek);
                ntotal += npeek;
            }
        }
        
        if (ntotal > 0 && ntotal == fsize) {
            fprintf(stdout, "recv done\n");
            ikcp_flush(kcp);    //force send ack.
            break;
        }

        if (brecv) {
            lastrecv = now;
        } else {
            if (now - lastrecv >= 10000) {
                fprintf(stderr, "exceeded 10s no data coming, network issue??\n");
                break;
            }
        }
    }
    
    close(ifd);
    close(sock);
    delete [] buf;
    freeaddrinfo(peer);
    delete user;
    
    return 0;
}
