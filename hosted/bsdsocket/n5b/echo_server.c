/* Minimal localhost TCP echo server on 127.0.0.1:12345 for the [N5b] proof. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
int main(void){
    int ls=socket(AF_INET,SOCK_STREAM,0); int one=1;
    setsockopt(ls,SOL_SOCKET,SO_REUSEADDR,&one,sizeof one);
    struct sockaddr_in a; memset(&a,0,sizeof a);
    a.sin_family=AF_INET; a.sin_port=htons(12345); a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if(bind(ls,(struct sockaddr*)&a,sizeof a)<0){perror("bind");return 1;}
    listen(ls,4);
    fprintf(stderr,"echo: listening 127.0.0.1:12345\n");
    for(;;){
        int c=accept(ls,0,0); if(c<0)continue;
        fprintf(stderr,"echo: client connected\n");
        char b[256]; ssize_t n;
        while((n=recv(c,b,sizeof b,0))>0){ ssize_t w=send(c,b,n,0); (void)w; fprintf(stderr,"echo: bounced %zd bytes\n",n); }
        close(c); fprintf(stderr,"echo: client closed\n");
    }
}
