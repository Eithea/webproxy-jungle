#include <stdio.h>
#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

void doit(int fd);
int parse_uri(char *uri, char *hostname, char *path, int *port);
void makeHTTPheader(char *http_header, char *hostname, char *path, int port, rio_t *client_rio);


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