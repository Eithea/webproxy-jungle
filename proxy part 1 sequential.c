#include <stdio.h>
#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

void doit(int fd);
int parse_uri(char *uri, char *hostname, char *path, int *port);
void makeHTTPheader(char *http_header, char *hostname, char *path, int port, rio_t *client_rio);

// 프록시 서버도 main의 알고리즘, doit의 상단부는 tiny와 같다
int main(int argc, char **argv)
{
    int listenfd, connfd;
    char hostname[MAXLINE], port[MAXLINE];
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    if (argc != 2)
    {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    listenfd = Open_listenfd(argv[1]);
    while (1)
    {
        clientlen = sizeof(clientaddr);
        connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0);
        printf("Accepted connection from (%s, %s)\n", hostname, port);
        doit(connfd);
        Close(connfd);
    }
    return 0;
}

void doit(int fd)
{
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char HTTPheader[MAXLINE], hostname[MAXLINE], path[MAXLINE];
    rio_t rio;
    int backfd;
    rio_t backrio;

    Rio_readinitb(&rio, fd);
    Rio_readlineb(&rio, buf, MAXLINE);
    printf("Request headers:\n");
    printf("%s", buf);
    sscanf(buf, "%s %s %s", method, uri, version);

    if (strcasecmp(method, "GET"))
    {
        printf("Proxy does not implement this method\n");
        return;
    }
    int port;
    // uri를 파싱하는 목적은 서버마다 다른데, 프록시 서버에서의 목적은 hostname과 path를 추출하고 포트를 결정하는 것이다
    // 아래에서 이 목적에 따르는 코드로 parse_uri를 구현
    parse_uri(uri, hostname, path, &port);
    // 결정된 hostname, path, port에 따라 HTTP header를 만든다
    makeHTTPheader(HTTPheader, hostname, path, port, &rio);

    char portch[10];
    sprintf(portch, "%d", port);
    // back과 연결 후 만든 HTTP header를 보낸다
    backfd = Open_clientfd(hostname, portch);
    if(backfd < 0)
    {
        printf("connection failed\n");
        return;
    }
    Rio_readinitb(&backrio, backfd);
    Rio_writen(backfd, HTTPheader, strlen(HTTPheader));
    // 이어서 클라이언트가 보냈던 원문도 보낸 뒤 close
    size_t n;
    while((n = Rio_readlineb(&backrio, buf, MAXLINE)) != 0)
    {
        printf("proxy received %d bytes, then send\n", n);
        Rio_writen(fd, buf, n);
    }
    Close(backfd);
}

int parse_uri(char *uri, char *hostname, char *path, int *port)
{
    // port는 uri에 지정이 있어 갱신하는 것이 아니라면 80을 쓸 것
    *port = 80;
    char *hostnameP = strstr(uri, "//");
    // uri에 '//'가 있다면 hostnameP는 // 다음 indext부터
    if (hostnameP != NULL)
    {
        hostnameP = hostnameP + 2;
    }
    // 없다면 uri의 처음부터
    else
    {
        hostnameP = uri;
    }
    // path는 hostnameP의 ':'부터
    char *pathP = strstr(hostnameP, ":");
     
    if(pathP != NULL)
    {
        // ':'가 있다면 이를 \0으로 변환해 hostnameP를 둘로 cut하고
        *pathP = '\0';
        // hostname은 cut의 앞부분
        sscanf(hostnameP, "%s", hostname);
        // port는 ':'의 바로 다음 숫자부분, path는 그 다음부터의 스트링
        sscanf(pathP + 1, "%d%s", port, path);
    }
    else
    {
        // ':'가 없다면 '/'를 찾는다 (포트가 지정되지 않았으니 포트 이전의 /를 기준으로 자름)
        pathP = strstr(hostnameP, "/");
        if(pathP != NULL)
        {
            // 마찬가지로 cut해서 앞 뒤로 hostname, path 결정, port는 지정하지 않았으니 80을 그대로 쓴다
            *pathP = '\0';
            sscanf(hostnameP, "%s", hostname);
            *pathP = '/';
            sscanf(pathP, "%s", path);
        }
        else
        {
            // ':'도 '/'도 없다면 hostname만 카피한다
            sscanf(hostnameP, "%s", hostname);
        }
    }
    return 0;
}

static const char *user_agent_header = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";
static const char *conn_header = "Connection: close\r\n";
static const char *prox_header = "Proxy-Connection: close\r\n";
static const char *host_header_format = "Host: %s\r\n";
static const char *requestlint_header_format = "GET %s HTTP/1.0\r\n";
static const char *endof_header = "\r\n";
static const char *connection_key = "Connection";
static const char *user_agent_key = "User-Agent";
static const char *proxy_connection_key = "Proxy-Connection";
static const char *host_key = "Host";
// 조건대로 헤더를 만듦
void makeHTTPheader(char *http_header, char *hostname, char *path, int port, rio_t *client_rio)
{
    char buf[MAXLINE], request_header[MAXLINE], other_header[MAXLINE], host_header[MAXLINE];
    sprintf(request_header, requestlint_header_format, path);
    while(Rio_readlineb(client_rio, buf, MAXLINE) > 0)
    {
        if(strcmp(buf, endof_header) == 0)
        {
            break;
        }
        if(!strncasecmp(buf, host_key, strlen(host_key)))
        {
            strcpy(host_header, buf);
            continue;
        }
        if(!strncasecmp(buf, connection_key, strlen(connection_key)) && !strncasecmp(buf, proxy_connection_key, strlen(proxy_connection_key)) && !strncasecmp(buf, user_agent_key, strlen(user_agent_key)))
        {
            strcat(other_header, buf);
        }
    }
    if(strlen(host_header) == 0)
    {
        sprintf(host_header, host_header_format, hostname);
    }
    sprintf(http_header, "%s%s%s%s%s%s%s", request_header, host_header, conn_header, prox_header, user_agent_header, other_header, endof_header);
    return ;
}