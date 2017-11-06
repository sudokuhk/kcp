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

#include <map>
#include <vector>

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
    int ofd;
    off_t fsize;
    bool berror;
    bool bfull;
    bool bdone;
} user_data;

static int on_send_udp(const char * data, int size, ikcpcb * kcp, void * user)
{
    user_data * sockinfo = static_cast<user_data *>(user);
    
    int nsend = (int)sendto(sockinfo->sock, data, size, 0,
                            sockinfo->peer, sockinfo->addrlen);
    if (nsend < 0 && errno != EAGAIN) {
        sockinfo->berror = true;
    }
    return nsend;
}

int main(int argc, char * argv[])
{
    if (argc < 2) {
        fprintf(stderr, "less arguments:%d, need 2\n", argc);
        fprintf(stderr, "%s port\n", argv[0]);
        exit(0);
    }
    
    struct addrinfo hints, *local;
    memset(&hints, 0, sizeof(struct addrinfo));
    
    hints.ai_flags      = AI_PASSIVE;
    hints.ai_family     = AF_INET;
    hints.ai_socktype   = SOCK_STREAM;
    hints.ai_socktype   = SOCK_DGRAM;
    
    if (0 != getaddrinfo(NULL, argv[1], &hints, &local)) {
        fprintf(stderr, "incorrect network address.\n");
        return 0;
    }
    
    int sock = socket(local->ai_family, local->ai_socktype, local->ai_protocol);
    freeaddrinfo(local);
    
    set_nonblock(sock);
    if (bind(sock, local->ai_addr, local->ai_addrlen) < 0) {
        fprintf(stderr, "bind sock error:%s\n", strerror(errno));
        return 0;
    }
    
    int maxfd = sock + 1;
    struct timeval tv = {0, 10000};
    char data[2048];
    struct sockaddr client;
    socklen_t socklen = 0;
    int nrecv;
    
    typedef std::map<IUINT32, ikcpcb *> KCPMAP;
    typedef std::map<IUINT32, user_data *> DATAMAP;
    KCPMAP m_mapKCP;
    DATAMAP m_mapData;
    
    while (true) {
        fd_set rdset;
        
        FD_ZERO(&rdset);
        FD_SET(sock, &rdset);
        
        int active = select(maxfd, &rdset, NULL, NULL, &tv);
        if (active < 0) {
            fprintf(stderr, "select error:%s\n", strerror(errno));
            break;
        } else if (active > 0) {
            if (FD_ISSET(sock, &rdset)) {
                int count = 10;
                socklen = sizeof(client);
                while (count -- > 0 &&
                       (nrecv = (int)recvfrom(sock, data, 2048, 0, &client, &socklen)) > 0) {
                    //fprintf(stdout, "recv size:%d\n", nrecv);
                    socklen = sizeof(client);
                    if (nrecv <= sizeof(IUINT32)) {
                        continue;
                    }
                    
                    int conv = ikcp_getconv(data);
                    
                    if (m_mapKCP.find(conv) == m_mapKCP.end()) {
                        fprintf(stdout, "create kcp for conv:%d\n", conv);
                        user_data * user = new user_data;
                        user->sock = sock;
                        user->peer = (struct sockaddr *)malloc(sizeof(struct sockaddr));
                        user->addrlen = socklen;
                        user->bfull = false;
                        user->bdone = false;
                        memcpy(user->peer, &client, sizeof(client));
                        
                        struct sockaddr_in * paddr = (struct sockaddr_in *)&client;
                        paddr = (struct sockaddr_in *)user->peer;
                        
                        ikcpcb * kcp = ikcp_create(conv, user);
                        ikcp_nodelay(kcp, 1, 10, 2, 1);
                        ikcp_setminrto(kcp, 10);
                        ikcp_setoutput(kcp, on_send_udp);
                        ikcp_wndsize(kcp, 1024, 1024);
                        
                        m_mapKCP[conv] = kcp;
                        m_mapData[conv] = user;
                    }
                    
                    KCPMAP::iterator it = m_mapKCP.find(conv);
                    if (it == m_mapKCP.end()) {
                        continue;
                    }
                    
                    ikcpcb * kcp = it->second;
                    ikcp_input(kcp, data, nrecv);
                    
                    int npeek = ikcp_peeksize(kcp);
                    if (npeek <= 0) {
                        continue;
                    }
                    
                    if (ikcp_recv(kcp, data, npeek) < 0) {
                        ikcp_release(kcp);
                        m_mapKCP.erase(it);
                    } else {
                        data[npeek] = '\0';
                        fprintf(stdout, "want file:%s\n", data);
                        
                        int ofd = open(data, O_RDONLY);
                        if (ofd < 0) {
                            break;
                        }
                        
                        struct stat sb;
                        if (fstat(ofd, &sb) < 0) {
                            fprintf(stdout, "stat file error:%s\n", strerror(errno));
                            close(ofd);
                            return -1;
                        }
                        
                        m_mapData[conv]->ofd = ofd;
                        m_mapData[conv]->fsize = sb.st_size;
                        
                        int fsize = (int)sb.st_size;
                        fprintf(stdout, "file <%s> size:%d\n", data, fsize);
                        fsize = htonl(fsize);
                        
                        ikcp_send(kcp, (char *)&fsize, sizeof(fsize));
                        ikcp_update(it->second, iclock());
                    }
                }
            }
        }
        
        std::vector<IUINT32> doneconv;
        {
            IUINT32 now = iclock();
            KCPMAP::iterator it;
            for (it = m_mapKCP.begin(); it != m_mapKCP.end(); ++ it) {
                user_data * user = m_mapData[it->first];
                ikcpcb * kcp = it->second;
                
                if (ikcp_waitsnd(kcp) >= 1024 * 2 ||
                    (user->bfull && ikcp_waitsnd(kcp) >= 1024 / 2)) {
                    if (ikcp_check(kcp, now) == now) {
                        ikcp_update(kcp, now);
                    }
                    user->bfull = true;
                    continue;
                }
                user->bfull = false;
                
                if (!user->bdone) {
                    for (int i = 0; i < 5; i++) {
                        int nread = (int)read(user->ofd, data, 2048);
                        if (nread > 0) {
                            //fprintf(stderr, "read %d\n", nread);
                            ikcp_send(kcp, data, nread);
                        } else {
                            fprintf(stdout, "read done\n");
                            user->bdone = true;
                            break;
                        }
                    }
                } else {
                    //fprintf(stdout, "wait snd:%d\n", ikcp_waitsnd(kcp));
                    if (ikcp_waitsnd(kcp) == 0) {
                        doneconv.push_back(it->first);
                        fprintf(stdout, "send done\n");
                    }
                }
                
                if (ikcp_check(kcp, now) == now) {
                    ikcp_update(kcp, now);
                }
            }
        }
        
        {
            for (size_t i = 0; i < doneconv.size(); i++) {
                fprintf(stderr, "release kcp, conv:%d\n", doneconv[i]);
                KCPMAP::iterator it = m_mapKCP.find(doneconv[i]);
                if (it != m_mapKCP.end()) {
                    ikcp_release(it->second);
                    m_mapKCP.erase(it);
                }
                
                DATAMAP::iterator dit = m_mapData.find(doneconv[i]);
                if (dit != m_mapData.end()) {
                    user_data * user = dit->second;
                    close(user->ofd);
                    free(user->peer);
                    delete user;
                    m_mapData.erase(dit);
                }
            }
        }
    }
    
    return 0;
}
