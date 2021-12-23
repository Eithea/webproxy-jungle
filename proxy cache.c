#include <stdio.h>
#include "csapp.h"

void cache_init();
void *thread_routine(void *connfdp);
void doit(int connfd);
int parse_uri(char *uri, char *hostname, char *path, int *port);
void makeHTTPheader(char *HTTPheader, char *hostname, char *path, int port, rio_t *client_rio);

// main function
// 프록시 서버도 main의 알고리즘, doit의 상단부는 tiny와 같으니 sequential한 파트는 주석 생략
int main(int argc, char **argv)
{
    int listenfd;
    char hostname[MAXLINE], port[MAXLINE];
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    pthread_t tid;
    // 캐시 ON
    cache_init(); 
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
        // 스레드마다 각각의 connfd를 유지하기 위해 연결마다 메모리를 할당하여 포인팅
        // (따로 안쓰고 thread_routine에서 바로 버리니까 할당 안하고 덮어씌워도 문제없을듯)
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
    // connfdp도 이미 connfd를 얻어 역할을 다했으니 반납
    Free(connfdp);
    // 이후 sequential과 같은 과정 진행
    doit(connfd);
    Close(connfd);
}

/////////////////// cache imp. part

#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
// 오브젝트 최대갯수는 캐시용량과 오브젝트 용량으로 결정된다
#define MAX_OBJECT_NUM ((int)(MAX_CACHE_SIZE / MAX_OBJECT_SIZE))

typedef struct 
{
    char cache_obj[MAX_OBJECT_SIZE];
    char cache_uri[MAXLINE];
    int order; // LRU order
    int alloc, read;
    // write, read 과정에서 스레드간의 충돌으로부터 보호할 세마포어 각 1개씩
    sem_t writesemap, readsemap;
} cache_block;

typedef struct
{
    cache_block cacheOBJ[MAX_OBJECT_NUM];
    int cache_num;
} Cache;

// cache 스트럭쳐의 초기값을 설정
Cache cache;
void cache_init()
{
    cache.cache_num = 0;
    int index = 0;
    for (; index < MAX_OBJECT_NUM; index = index + 1)
    {
        cache.cacheOBJ[index].order = 0;
        cache.cacheOBJ[index].alloc = 0;
        Sem_init(&cache.cacheOBJ[index].writesemap, 0, 1);
        Sem_init(&cache.cacheOBJ[index].readsemap, 0, 1);
        cache.cacheOBJ[index].read = 0;
    }
}

// 캐시를 읽기 전 세마포어 연산으로 타 스레드로부터 보호
void readstart(int index)
{
    P(&cache.cacheOBJ[index].readsemap);
    cache.cacheOBJ[index].read = cache.cacheOBJ[index].read + 1;
    // +1한 read가 1이라면 이 스레드에서 readstart 이후 write할 가능성이 열려있다
    // 타 스레드의 write로부터도 보호
    if (cache.cacheOBJ[index].read == 1)
        P(&cache.cacheOBJ[index].writesemap);
    V(&cache.cacheOBJ[index].readsemap);
}

// readstart의 역연산으로 돌려놓음
void readend(int index)
{
    P(&cache.cacheOBJ[index].readsemap);
    cache.cacheOBJ[index].read = cache.cacheOBJ[index].read - 1;
    // -1한 read가 0이라면 readstart때 write 보호를 받은 오브젝트인데, 이제 다음 readstart 전까지 write할 가능성이 없다
    // write로부터 보호 해제
    if (cache.cacheOBJ[index].read == 0)
        V(&cache.cacheOBJ[index].writesemap);
    V(&cache.cacheOBJ[index].readsemap);
}

// 가용한 캐시가 있는지 탐색하고 있다면 index를 리턴하는 함수
int cache_find(char *uri)
{
    int index = 0;
    // 각 캐시에 대해 탐색
    for (; index < MAX_OBJECT_NUM; index = index + 1)
    {
        // 탐색 전 세마포어 보호
        readstart(index);
        if (cache.cacheOBJ[index].alloc && (strcmp(uri, cache.cacheOBJ[index].cache_uri) == 0))
            break;
        // 가용하다면 보호를 유지한 채 break, 아니라면 보호를 풀고 다음 인덱스로
        readend(index);
    }
    // 가용한 캐시가 없다면 -1을 return
    if (index >= MAX_OBJECT_NUM)
        return -1;
    return index;
}

// 빈 캐시, 혹은 LRU order가 제일 낮은 캐시를 골라 index를 리턴하는 함수
int cache_eviction()
{
    //minorder는 upper bound에서 시작해서 자신보다 낮은 값이 나올때마다 갱신된다
    int minorder = MAX_OBJECT_NUM + 1;
    int minindex = 0;
    int index = 0;
    // 모든 index를 탐색하며 비교
    for (; index < MAX_OBJECT_NUM; index = index + 1)
    {
        readstart(index);
        // 빈 캐시를 발견하면 탐색을 중단하고 index를 retrun
        if (!cache.cacheOBJ[index].alloc)
        {
            readend(index);
            return index;
        }
        // 빈 캐시가 발견되지 않는 동안 minorder를 갱신하며 탐색
        if (cache.cacheOBJ[index].order < minorder)
        {
            minindex = index;
            minorder = cache.cacheOBJ[index].order;
        }
    readend(index);
    }
    // 빈 캐시가 발견되지 않고 for문이 종료되었다면 minindex를 return
    return minindex;
}

// LRU order를 재정렬하는 함수
void cache_reorder(int target)
{
    // 방금 쓴 target을 최고 order로
    cache.cacheOBJ[target].order = MAX_OBJECT_NUM + 1;
    int index = 0;
    for (; index < MAX_OBJECT_NUM; index = index + 1)
    {
        // 나머지는 모두 order -1 (값을 수정하니 write 보호 필요)
        if (index - target)
        {
            P(&cache.cacheOBJ[index].writesemap);
            cache.cacheOBJ[index].order = cache.cacheOBJ[index].order - 1;
            V(&cache.cacheOBJ[index].writesemap);
        }
    }
}

// cache_eviction으로 차출된 캐시에 uri와 buf를기록하는 함수
void cache_uri(char *uri, char *buf)
{
    // 차출
    int index = cache_eviction();
    // 쓰기 전 세마포어 보호
    P(&cache.cacheOBJ[index].writesemap);
    // buf, uri 카피
    strcpy(cache.cacheOBJ[index].cache_obj, buf);
    strcpy(cache.cacheOBJ[index].cache_uri, uri);
    cache.cacheOBJ[index].alloc = 1;
    // LRU order 재정렬
    cache_reorder(index);
    // 보호 해제
    V(&cache.cacheOBJ[index].writesemap);
}

/////////////////// cache imp. part end

void doit(int connfd)
{
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char HTTPheader[MAXLINE], hostname[MAXLINE], path[MAXLINE];
    int backfd;
    rio_t rio, backrio;
    
    Rio_readinitb(&rio, connfd);
    Rio_readlineb(&rio, buf, MAXLINE);
    printf("Request headers:\n");
    printf("%s", buf);
    sscanf(buf, "%s %s %s", method, uri, version);

    if (strcasecmp(method, "GET"))
    {
        printf("Proxy does not implement this method\n");
        return;
    }

    // uri를 MAX_OBJECT_SIZE가 허용하는 만큼만 uri_store에 담는다
    char uri_store[MAX_OBJECT_SIZE];
    strcpy(uri_store, uri);
    int cache_index;
    // 캐시에 있는지 확인
    if ((cache_index = cache_find(uri_store)) != -1)
    {
        // 있다면 캐시에서 보내고 doit 종료
        readstart(cache_index);
        Rio_writen(connfd, cache.cacheOBJ[cache_index].cache_obj, strlen(cache.cacheOBJ[cache_index].cache_obj));
        readend(cache_index);
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
    char cachebuf[MAX_OBJECT_SIZE];
    size_t sizerecvd, sizebuf = 0;
    while((sizerecvd = Rio_readlineb(&backrio, buf, MAXLINE)) != 0)
    {
        // 이때 buf의 크기가 MAX_OBJECT_SIZE보다 작다면 cachebuf에 카피하고
        sizebuf = sizebuf + sizerecvd;
        if (sizebuf < MAX_OBJECT_SIZE)
        {
            strcat(cachebuf, buf);
        }
        printf("proxy received %d bytes, then send\n", sizerecvd);
        Rio_writen(connfd, buf, sizerecvd);
    }
    Close(backfd);
    if (sizebuf < MAX_OBJECT_SIZE)
    {
        // cachebuf를 cache에 기록한다
        cache_uri(uri_store, cachebuf);
    }
}

// uri로부터 hostname, path를 파싱하고 port를 결정하는 함수
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
// 조건대로 포맷을 맞춰 헤더를 만드는 함수
void makeHTTPheader(char *HTTPheader, char *hostname, char *path, int port, rio_t *client_rio)
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
    sprintf(HTTPheader, "%s%s%s%s%s%s%s", request_header, host_header, conn_header, prox_header, user_agent_header, other_header, endof_header);
}