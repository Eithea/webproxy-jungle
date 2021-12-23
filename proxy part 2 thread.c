#include <stdio.h>
#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

void doit(int fd);
int parse_uri(char *uri, char *hostname, char *path, int *port);
void makeHTTPheader(char *http_header, char *hostname, char *path, int port, rio_t *client_rio);
void *thread_routine(void *connfdp);

int main(int argc, char **argv)
{
    int listenfd;
    char hostname[MAXLINE], port[MAXLINE];
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    pthread_t tid;
    if (argc != 2)
    {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    listenfd = Open_listenfd(argv[1]);
    while (1)
    {
        // sever main에서 일련의 처리를 하는 대신 스레드를 분기
        // 각 연결에 대해 이후의 과정은 스레드 내에서 병렬적으로 처리되며
        // main은 다시 while문의 처음으로 돌아가 새로운 연결을 기다린다
        clientlen = sizeof(clientaddr);
        // 이때 각 스레드는 모두 각각의 connfd를 가져야 하기 때문에, 연결마다 메모리를 할당하여 포인팅한다
        int *connfdp = Malloc(sizeof(int));
        *connfdp = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0);
        printf("Accepted connection from (%s, %s)\n", hostname, port);
        // thread_routine를 실행하는 thread 생성
        // 연결마다 고유한 connfdp를 thread_routine의 입력으로 가져간다
        Pthread_create(&tid, NULL, thread_routine, connfdp);
    }
    return 0;
}

void *thread_routine(void *connfdp)
{
    // 각 스레드별 connfd는 입력으로 가져온 connfdp가 가리키던 할당된 위치의 fd값
    int connfd = *((int *)connfdp);
    // 스레드 종료시 자원을 반납하고
    Pthread_detach(pthread_self());
    // connfdp도 이미 connfd를 얻어 역할을 다했으니 반납한다
    Free(connfdp);
    doit(connfd);
    Close(connfd);
    return NULL;
}

// 이후 각 스레드의 수행은 seqential과 같으니 생략

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
    parse_uri(uri, hostname, path, &port);
    makeHTTPheader(HTTPheader, hostname, path, port, &rio);

    char portch[10];
    sprintf(portch, "%d", port);
    backfd = Open_clientfd(hostname, portch);
    if(backfd < 0)
    {
        printf("connection failed\n");
        return;
    }
    Rio_readinitb(&backrio, backfd);
    Rio_writen(backfd, HTTPheader, strlen(HTTPheader));

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
    *port = 80;
    char *hostnameP = strstr(uri, "//");
    if (hostnameP != NULL)
    {
        hostnameP = hostnameP + 2;
    }
    else
    {
        hostnameP = uri;
    }
    char *pathP = strstr(hostnameP, ":");
    if(pathP != NULL)
    {
        *pathP = '\0';
        sscanf(hostnameP, "%s", hostname);
        sscanf(pathP + 1, "%d%s", port, path);
    }
    else
    {
        pathP = strstr(hostnameP, "/");
        if(pathP != NULL)
        {
            *pathP = '\0';
            sscanf(hostnameP, "%s", hostname);
            *pathP = '/';
            sscanf(pathP, "%s", path);
        }
        else
        {
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
        if(!strncasecmp(buf, connection_key, strlen(connection_key))
                &&!strncasecmp(buf, proxy_connection_key, strlen(proxy_connection_key))
                &&!strncasecmp(buf, user_agent_key, strlen(user_agent_key)))
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