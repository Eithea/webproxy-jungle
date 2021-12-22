/* $begin tinymain */
/*
 * tiny.c - A simple, iterative HTTP/1.0 Web server that uses the
 *     GET method to serve static and dynamic content.
 *
 * Updated 11/2019 droh
 *   - Fixed sprintf() aliasing issue in serve_static(), and clienterror().
 */
#include "csapp.h"

void doit(int fd);
void read_requesthdrs(rio_t *rp);
int parse_uri(char *uri, char *filename, char *cgiargs);
void serve_static(int fd, char *filename, int filesize);
void get_filetype(char *filename, char *filetype);
void serve_dynamic(int fd, char *filename, char *cgiargs);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);

// ./tiny 8000 실행시 argv[0] = ./tiny, argv[1] = 8000, argc = 2
int main(int argc, char **argv)
{
    int listenfd, connfd;
    char hostname[MAXLINE], port[MAXLINE];
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;

    // argc != 2라면 (= ./tiny <port번호> 꼴의 실행이 아니라면)
    if (argc != 2)
    {
        // 포트번호를 입력하라고 메세지를 출력
        // ./tiny (포트번호없이) 실행하면 나옴
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    // 입력받은 포트 번호(8000)로 open_listenfd를 실행
    // listenfd는 setsocket, bind, listen을 거친 오픈된 듣기 전용 소켓의 식별자
    listenfd = Open_listenfd(argv[1]);

    // 서버 프로세스 종료 전까지 무한히 대기
    while (1)
    {
        // 클라이언트와의 연결
        clientlen = sizeof(clientaddr);
        connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        // clientaddr로부터 hostname과 port를 추출하여 아래 print문 출력
        Getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE, 0);
        printf("Accepted connection from (%s, %s)\n", hostname, port);
        // connfd와의 트랜잭션 수행
        doit(connfd);
        // 연결 종료 후 while문 시작부로 회귀
        Close(connfd);
    }
}

// connfd와의 트랜잭션을 수행하는 함수
void doit(int fd)
{
    int is_static;
    struct stat sbuf;
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char filename[MAXLINE], cgiargs[MAXLINE];
    rio_t rio;

    // robust I/O가 읽은 클라이언트 fd로부터의 트랜잭션을 &rio에 기록
    Rio_readinitb(&rio, fd);
    // 라인을 읽어 buf에 저장
    // buf = "GET / HTTP/1.1", "GET /godzilla.gif HTTP/1.1" etc...
    Rio_readlineb(&rio, buf, MAXLINE);
    
    // "GET" "/godzilla.gif" "HTTP/1.1" 가 각각 method, uri, version에 대입
    printf("Request headers:\n");
    printf("%s", buf);
    sscanf(buf, "%s %s %s", method, uri, version);

    // method가 GET이 아니라면
    if (strcasecmp(method, "GET"))
    {
        // tiny에서 지원하지 않는 메소드이므로 error 출력 후 return
        clienterror(fd, method, "501", "Not implemented", "Tiny does not implement this method");
        return;
    }

    // HTTP header는 지원하지 않으므로 읽기만 하고 흘린다 (아무것도 안함)
    // (숙제문제 11.11의 요구사항)
    read_requesthdrs(&rio);

    // uri로부터 filename과 cgiargs에 값을 넣는 parse_uri 실행
    // 실행 후 정적/동적 여부에 따라 return된 1 or 0을 is_static에 대입
    is_static = parse_uri(uri, filename, cgiargs);

    // filename으로부터 stat 구조체 sbuf를 생성
    if (stat(filename, &sbuf) < 0)
    {
        // 실패했다면 (stat에서 return -1이 나왔다면) 에러 출력 후 return
        clienterror(fd, filename, "404", "Not found", "Tiny couldn’t find this file");
        return;
    }
    // is_static이 1이라면 (= 정적 페이지라 parse_uri가 1을 return했다면)
    if (is_static)
    {
        // S_ISREG : 정규 파일인지 판별, S_IRUSR : 읽기 권한이 있는지 판별
        // 둘 다 만족하는 경우에만 파일을 읽을 수 있으니, 그렇지 않다면 에러 출력 후 return
        if (!(S_ISREG(sbuf.st_mode)) || !(S_IRUSR & sbuf.st_mode))
        {
            clienterror(fd, filename, "403", "Forbidden", "Tiny couldn’t read the file");
            return;
        }
        // 읽을 수 있는 파일이라면 정적 페이지를 생성하는 serve_static 실행
        serve_static(fd, filename, sbuf.st_size);
    }
    // is_static = 0, 동적 페이지라면
    else
    {
        // 마찬가지로 정규파일, 권한 여부 판단 뒤 불가능하면 에러 출력 후 return
        if (!(S_ISREG(sbuf.st_mode)) || !(S_IXUSR & sbuf.st_mode))
        {
            clienterror(fd, filename, "403", "Forbidden", "Tiny couldn’t run the CGI program");
            return;
        }
        // 동적 페이지를 생성하는 serve_dynamic 실행
        serve_dynamic(fd, filename, cgiargs);
    }
}

// HTTP 헤더들 그냥 읽고 아무것도 안함 (출력만 함)
// 11.11을 풀려면 구현 필요
void read_requesthdrs(rio_t *rp)
{
    char buf[MAXLINE];
    Rio_readlineb(rp, buf, MAXLINE);

    while (strcmp(buf, "\r\n"))
    {
        Rio_readlineb(rp, buf, MAXLINE);
        printf("%s", buf);
    }
    return;
}

// URI를 파싱해서 선언만 해둔 빈 변수 filename과 cgiargs에 값을 넣는 함수
// 정적 페이지라면 작업 후 return 1, 동적 페이지라면 작업 후 return 0
// 이 리턴값을 이용해 이후 doit에서 실행할 구문을 분기
int parse_uri(char *uri, char *filename, char *cgiargs)
{
    char *ptr;
    // URI에 cgi-bin이 없다면 정적 페이지로 표시
    // cgi-bin가 없다고 다 정적 페이지인건 아닌데 tiny는 CGI만 지원
    if (!strstr(uri, "cgi-bin"))
    {
        //정적페이지에 cgiargs는 필요없으므로 ""
        // filename은 '.' + uri (./godzilla.gif)
        strcpy(cgiargs, "");
        strcpy(filename, ".");
        strcat(filename, uri);

        // URI가 요구하는 파일 없이 그냥 /이면 home을 띄우도록
        if (uri[strlen(uri) - 1] == '/')
        {
            strcat(filename, "home.html");
        }
        // 정적 페이지임을 의미하는 return 1
        return 1;
    }

    // cgi-bin이 있다면 CGI를 지원
    else
    {
        // uri에 ?가 있다면
        ptr = index(uri, '?');
        if (ptr)
        {
            // cgiargs에 ? 다음글자의 index를 대입
            strcpy(cgiargs, ptr + 1);
            *ptr = '\0';
        }
        else
        {
            // 없다면 cgiargs는 ""
            strcpy(cgiargs, "");
        }
        // filename은 마찬가지로 '.' + uri
        strcpy(filename, ".");
        strcat(filename, uri);
        // 동적 페이지임을 의미하는 return 0
        return 0;
    }
}

// filename에서 확장자를 파싱해서 filetype을 결정하는 함수
void get_filetype(char *filename, char *filetype)
{
    if (strstr(filename, ".html"))
        strcpy(filetype, "text/html");
    else if (strstr(filename, ".gif"))
        strcpy(filetype, "image/gif");
    else if (strstr(filename, ".png"))
        strcpy(filetype, "image/png");
    else if (strstr(filename, ".jpg"))
        strcpy(filetype, "image/jpeg");
    // 11.7 mp4 확장자의 video filetype 지원
    else if (strstr(filename, ".mp4"))
        strcpy(filetype, "video/mp4");
    else
        strcpy(filetype, "text/plain");
}

// 정적 페이지를 구성해서 fd에 보내는 함수
void serve_static(int fd, char *filename, int filesize)
{
    int srcfd;
    char *srcp, filetype[MAXLINE], buf[MAXBUF], *fbuf;

    // 트랜잭션에서 클라이언트에 보낼 콘텐츠 인포를 프린트하면서 buf에 저장
    get_filetype(filename, filetype);
    sprintf(buf, "HTTP/1.0 200 OK\r\n");
    sprintf(buf, "%sServer: Tiny Web Server\r\n", buf);
    sprintf(buf, "%sConnection: close\r\n", buf);
    sprintf(buf, "%sContent-length: %d\r\n", buf, filesize);
    sprintf(buf, "%sContent-type: %s\r\n\r\n", buf, filetype);

    // buf를 fd에 보냄
    Rio_writen(fd, buf, strlen(buf));
    printf("Response headers:\n");
    printf("%s", buf);

    // 파일을 읽기 전용으로 오픈
    srcfd = Open(filename, O_RDONLY, 0);
    // 11.9
    // srcp = Mmap(0, filesize, PROT_READ, MAP_PRIVATE, srcfd, 0);
    // Close(srcfd);
    // Rio_writen(fd, srcp, filesize);
    // Munmap(srcp, filesize);

    // 11.9
    // malloc을 이용하여 filesize만큼 할당
    // 할당된 메모리에 파일 표시
    fbuf = malloc(filesize);
    // srcfd로 대입된 파일을 fbuf에 기록
    Rio_readn(srcfd, fbuf, filesize);
    // 열린 파일은 다 썼으니 닫음
    Close(srcfd);
    // fbuf에 기록된 파일을 fd에 보냄
    Rio_writen(fd, fbuf, filesize);
    // fbuf도 다 썼으니 free
    free(fbuf);
}

// 동적 페이지를 구성해서 fd에 보내는 함수
void serve_dynamic(int fd, char *filename, char *cgiargs)
{
    char buf[MAXLINE], *emptylist[] = {NULL};

    // 마찬가지로 프린트하면서 buf에 저장하고 fd에 보냄
    sprintf(buf, "HTTP/1.0 200 OK\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Server: Tiny Web Server\r\n");
    Rio_writen(fd, buf, strlen(buf));

    // Fork()의 return값이 0이라면 (= 자식 프로세스 생성이 성공했다면)
    if (Fork() == 0)
    {
        // 환경변수 environ 설정하고 파일 디스크립터를 복제한다는데
        // 시스템수준은 모르겠고 그냥 그런가보다 싶다
        // 아무튼 cgiargs를 따르는 자식 프로세스를 실행하는 과정
        setenv("QUERY_STRING", cgiargs, 1);
        Dup2(fd, STDOUT_FILENO);
        // filename이 가리키는 파일 실행
        Execve(filename, emptylist, environ);
    }
    // 자식 프로세스가 종료되면 끝
    Wait(NULL);
}

// 에러 발생시 실행하는 함수
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg)
{
    char buf[MAXLINE], body[MAXBUF];
    // 에러 내용 출력하고 body, buf에 저장해서 클라이언트에 보냄
    sprintf(body, "<html><title>Tiny Error</title>");
    sprintf(body, "%s<body bgcolor=" "ffffff" ">\r\n", body);
    sprintf(body, "%s%s: %s\r\n", body, errnum, shortmsg);
    sprintf(body, "%s<p>%s: %s\r\n", body, longmsg, cause);
    sprintf(body, "%s<hr><em>The Tiny Web Server</em>\r\n", body);
    sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-type: text/html\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body));
    Rio_writen(fd, buf, strlen(buf));
    Rio_writen(fd, body, strlen(body));
}