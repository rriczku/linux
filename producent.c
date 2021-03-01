#define _GNU_SOURCE
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <sys/timerfd.h>
#include <sys/epoll.h>
#include <poll.h>
#include <signal.h>
#include <fcntl.h>

#define BLOCKSIZE 640
#define MAXEVENTS 1024
#define SELLINGPACK 13312


struct parameters{
    float p; //tempo wytwarzania materialu
    struct sockaddr_in a;
};

struct myepoll{
    int fd;
    char* ip;
    int port;
    int bytesread;
};

struct circularBuffer
{
    int first;
    int last;
    int size;
    int currentSize;
    struct myepoll* buffer;
};
struct circularBuffer* create(int size);
void destroy(struct circularBuffer* buffer);
int add(struct circularBuffer* buffer,struct myepoll element);
struct myepoll pop(struct circularBuffer* buffer);
int getCurrentSize(struct circularBuffer* buffer);


float parseFloat(char* s);
int parseInt(char* s);
struct parameters parseParams(int argc, char** argv);
void adressParsing(char* s, struct parameters* p);
int startServer(struct sockaddr_in addr);
int buildStorage(float p,int sSocket);
void production(float time, int pipefd);
void distribution(int pipefd,int sSocket);
int createEpoll(struct epoll_event* event,int sSocket,int timerds);
int acceptClient(int sfd);
void getPeerAddress(int socket,char** address,int* port);
int createTimer();
void sellingJob(int pipefd, struct epoll_event event,int* nofclients,int* reserved);
void printFiveSecondReport(int pipefd, int nofclients,int* lastcap);
int getCurrentPipeSize(int pipefd);
int handleQueue(struct epoll_event* event,struct circularBuffer* cb, int epfd,int freeBytes);
void readAndWaste(int pipefd,int size);
char* getTS();

int main(int argc,char* argv[]) {
   struct parameters parameters=parseParams(argc,argv);

   int sSocket=startServer(parameters.a);

   if(signal(SIGPIPE,SIG_IGN)==SIG_ERR)
   {
       perror("Signal error");
       exit(EXIT_FAILURE);
   }
    if(signal(SIGCHLD,SIG_IGN)==SIG_ERR)
    {
        perror("Signal error");
        exit(EXIT_FAILURE);
    }

    buildStorage(parameters.p,sSocket);
   return 0;
}

struct parameters parseParams(int argc, char** argv)
{
    int res;
    struct parameters ret;
    int opt;
    res = inet_aton("127.0.0.1", &ret.a.sin_addr);
    if(res==-1)
    {
        perror("Error inet_aton1\n");
        exit(EXIT_FAILURE);
    }
    while((opt=getopt(argc,argv,"p:"))!=-1)
    {
                if(opt=='p')
                ret.p=parseFloat(optarg)*2662;
    }
    if (optind < argc) {
        while (optind < argc) {
            adressParsing(argv[optind++],&ret);
        }
    }
    return ret;
}

int parseInt(char* s)
{
    char* end;
    errno=0;
    long val=strtol(s,&end,10);

    int ret=(int)val;
    if(errno==ERANGE||ret!=val||*end!='\0')
    {
        perror("ERROR PARSEINT\n");
        exit(EXIT_FAILURE);
    }
    return ret;
}

float parseFloat(char* s)
{
    char* end;
    errno=0;
    float ret = strtof(optarg,&end);

    if(errno==ERANGE||*end!='\0')
    {
        perror("Error parseFloat\n");
        exit(EXIT_FAILURE);
    }
    return ret;
}

void adressParsing(char* s, struct parameters* p)
{
    char* t1;
    char* t2;
    t1=strtok(s,"[]:");
    if(t1==NULL) {
        perror("ERROR STRTOK\n");
        exit(EXIT_FAILURE);
    }
    t2=strtok(NULL,"[]:");
    if(t2!=NULL)
    {
        int r=inet_aton(t1,&p->a.sin_addr);
        if(r==0)
        {
            perror("ERROR INET_ATON\n");
            exit(EXIT_FAILURE);
        }
        p->a.sin_port=htons(parseInt(t2));
    }
    else
    {
        p->a.sin_port=htons(parseInt(t1));
    }
}

int startServer(struct sockaddr_in addr)
{
    int sSocket=socket(AF_INET,SOCK_STREAM,0);
    if(sSocket==-1)
    {
        perror("Error socket!\n");
        exit(EXIT_FAILURE);
    }
    addr.sin_family=AF_INET;

    if(bind(sSocket,(struct sockaddr*)&addr,sizeof(struct sockaddr_in))==-1)
    {
        perror("Error bind\n");
        exit(EXIT_FAILURE);
    }

    if(listen(sSocket,32)==-1)
    {
        perror("Error listen\n");
        exit(EXIT_FAILURE);
    }

    return sSocket;
}

void production(float time, int pipefd)
{
    char character=65;
    char* buffer=(char*)calloc(BLOCKSIZE,sizeof(char));
    size_t nanoseconds=(BLOCKSIZE/time)*(size_t)1000000000;
    struct timespec ts={};
    ts.tv_sec=nanoseconds/1000000000;
    ts.tv_nsec=nanoseconds%(size_t)1000000000;
    int cap = fcntl(pipefd, F_GETPIPE_SZ);

    int capacity=0;

    for(;;)
    {
        do {
            capacity=getCurrentPipeSize(pipefd);
        }while(cap-capacity<BLOCKSIZE);

            if(character==91) character=97;
            else if(character==123) character=65;
            memset(buffer,character,BLOCKSIZE);
            if(write(pipefd,buffer,BLOCKSIZE)==-1)
            {
                if(errno==EPIPE) {
                    perror("Error write to parent\n");
                    close(pipefd);
                    exit(EXIT_FAILURE);
                }
            }
            character++;
            nanosleep(&ts,NULL);
    }
}

int buildStorage(float p,int sSocket)
{
    int pipefd[2];
    pid_t pid;
    int res=pipe(pipefd);
   // fcntl(res, F_SETFL, O_NONBLOCK);
    if(res==-1)
    {
        perror("Error pipe\n");
        exit(EXIT_FAILURE);
    }
    pid=fork();
    if(pid==-1)
    {
        perror("Error fork\n");
        exit(EXIT_FAILURE);
    }
    else if(pid==0)
    {
        //child writes to parent
        close(pipefd[0]);
        production(p,pipefd[1]);
    }
    else
    {
        close(pipefd[1]);
        distribution(pipefd[0],sSocket);
    }
    return 1;
}

void distribution(int pipefd,int sSocket)
{
    int lastcap=0;
    int nofclients=0;
    int reservedData=0;
    int epoll_fd,event_count;
    struct epoll_event event,events[MAXEVENTS];
    uint64_t exp;
    int timerds=createTimer();
    struct circularBuffer* cb=create(MAXEVENTS); //buffor na klientow dla ktorych aktualnie nie ma bajtow
    epoll_fd=createEpoll(&event,sSocket,timerds);
    for(;;)
    {
        reservedData+=(handleQueue(&event,cb,epoll_fd,getCurrentPipeSize(pipefd)-reservedData)*SELLINGPACK);
        event_count=epoll_wait(epoll_fd,events,MAXEVENTS,0);
        if(event_count==-1)
        {
            perror("error epoll_wait\n");
            exit(EXIT_FAILURE);
        }
        for(int i=0;i<event_count;i++)
        {
            struct myepoll* mep1=(struct myepoll*)events[i].data.ptr;
            if((events[i].events & EPOLLERR) || (events[i].events & EPOLLRDHUP)) //klient sie rozlaczyl
            {
                char* ts=getTS();
                struct myepoll* tempsocket=(struct myepoll*)events[i].data.ptr;
                fprintf(stderr,"%s Connection with %s %d lost. Lost bytes: %d\n",ts,tempsocket->ip,tempsocket->port,SELLINGPACK-tempsocket->bytesread);
                if(tempsocket->bytesread>0)
                {
                    readAndWaste(tempsocket->fd,SELLINGPACK-tempsocket->bytesread);
                    reservedData-=(SELLINGPACK-tempsocket->bytesread);
                }
                nofclients--;
                close(tempsocket->fd);
            }
            else if(mep1->fd==timerds) //timer
            {
                if(read(timerds,&exp,sizeof(uint64_t))!=sizeof(uint64_t))
                {
                    perror("error read timer\n");
                    exit(EXIT_FAILURE);
                }
                printFiveSecondReport(pipefd,nofclients,&lastcap);
            }
            else if(mep1->fd==sSocket) // nowe polaczenie
            {
                int newClient=acceptClient(sSocket);
                if(nofclients>MAXEVENTS){
                    close(newClient);
                    continue;
                }
                nofclients++;
                char* ip=(char*)calloc(15,sizeof(char));
                int tempport=0;
                getPeerAddress(newClient,&ip,&tempport);
                struct myepoll* mep=(struct myepoll*)calloc(1,sizeof(struct myepoll));
                mep->fd=newClient;
                mep->bytesread=0;
                mep->ip=ip;
                mep->port=tempport;

                if((getCurrentPipeSize(pipefd)-reservedData<SELLINGPACK)||(getCurrentSize(cb)>0)) //jesli nie mamy zasobow lub buffor cykliczny zawiera w srodku klienta to nowego wrzucamy do buffora
                {
                    add(cb,*mep);
                    continue;
                }

                reservedData+=SELLINGPACK;
                event.data.ptr=mep;
                event.events=EPOLLOUT | EPOLLRDHUP;
                if(epoll_ctl(epoll_fd,EPOLL_CTL_ADD,newClient,&event)==-1)
                {
                    perror("Error epoll_ctl2\n");
                    exit(EXIT_FAILURE);
                }
                continue;
            }
            else // sprzedaz
            {
                sellingJob(pipefd,events[i],&nofclients,&reservedData);
            }
        }
    }
}

int createEpoll(struct epoll_event* event,int sSocket,int timerds)
{
    struct myepoll* one=(struct myepoll*)calloc(1,sizeof(struct myepoll));
    struct myepoll* two=(struct myepoll*)calloc(1,sizeof(struct myepoll));

    int epoll_fd=epoll_create1(0);
    if(epoll_fd==-1)
    {
        perror("epoll create error\n");
        exit(EXIT_FAILURE);
    }
    one->fd=sSocket;
    event->data.ptr=(void*)one;
    event->events=EPOLLIN;
    if(epoll_ctl(epoll_fd,EPOLL_CTL_ADD,sSocket,event)==-1)
    {
        perror("error epoll_ctl1\n");
        exit(EXIT_FAILURE);
    }
    two->fd=timerds;
    event->data.ptr=(void*)two;
    event->events=EPOLLIN;
    if(epoll_ctl(epoll_fd,EPOLL_CTL_ADD,timerds,event)==-1)
    {
        perror("error epoll_ctl13\n");
        exit(EXIT_FAILURE);
    }
    return epoll_fd;
}

int acceptClient(int sfd)
{
    int newClient=accept4(sfd,NULL,NULL,O_NONBLOCK);
    if(newClient==-1)
    {
        perror("error accept\n");
        exit(EXIT_FAILURE);
    }
    return newClient;
}

void getPeerAddress(int socket,char** address, int* port)
{
    struct sockaddr_in addr;
    unsigned int addrlen=sizeof(addr);
    if(getpeername(socket,&addr,&addrlen)==-1)
    {
        perror("Error getpeername\n");
        exit(EXIT_FAILURE);
    }
    *port=addr.sin_port;
    strcpy(*address,inet_ntoa(addr.sin_addr));
}

int createTimer()
{
    struct itimerspec its;
    its.it_value.tv_sec = 5;
    its.it_value.tv_nsec = 0;
    its.it_interval.tv_sec = 5;
    its.it_interval.tv_nsec = 0;
    int timer=timerfd_create(CLOCK_MONOTONIC,TFD_NONBLOCK);
    if(timer==-1)
    {
        perror("Error timerfd_create\n");
        exit(EXIT_FAILURE);
    }
    if(timerfd_settime(timer,0,&its,NULL)==-1)
    {
        perror("Error timerfd_settime\n");
        exit(EXIT_FAILURE);
    }
    return timer;
}

void sellingJob(int pipefd, struct epoll_event event,int* nofclients,int* reserved)
{
    struct myepoll* mep=(struct myepoll*)event.data.ptr;
    if(mep->bytesread>=SELLINGPACK)
    {
        char* ts=getTS();
        (*nofclients)--;
        close(mep->fd);
        fprintf(stderr, "%s Client %s %d disconnected. Bytes lost: %d\n", ts, mep->ip, mep->port,SELLINGPACK-mep->bytesread);
    }else{
        char* buffor=(char*)calloc(SELLINGPACK/4,sizeof(char));
        int bytesRead=read(pipefd,buffor,SELLINGPACK/4);
        if(bytesRead==-1)
        {
            perror("ERROR reading from pipe\n");
            exit(EXIT_FAILURE);
        }
        (*reserved)-=bytesRead;
        int bytesWritten=write(mep->fd,buffor,bytesRead);
        if(bytesWritten==-1)
        {
            readAndWaste(pipefd,SELLINGPACK-mep->bytesread); //doczytanie danych z pipe
            (*reserved)-=SELLINGPACK-mep->bytesread;
            (*nofclients)--;
            close(mep->fd);
        }
        else{
            mep->bytesread+=bytesWritten;
        }
    }
}

void printFiveSecondReport(int pipefd, int nofclients,int* lastcap)
{
    int cap = fcntl(pipefd, F_GETPIPE_SZ); //pojemnosc magazynu
    int capacity=getCurrentPipeSize(pipefd);
    float percentage=(float)((float)capacity/(float)cap)*100;
    int flow = abs(*lastcap-capacity);
    *lastcap=capacity;
    char* ts=getTS();
    fprintf(stderr,"%s Number of clients : %d usage: %d (%.2f%%) Flow since last report: %d\n\n",ts,nofclients,capacity,percentage,flow);
}

int getCurrentPipeSize(int pipefd)
{
    int ret=0;
    if(ioctl(pipefd,FIONREAD,&ret)==-1)
    {
        perror("Error ioctl\n");
        exit(EXIT_FAILURE);
    }
    return ret;
}

int handleQueue(struct epoll_event* event,struct circularBuffer* cb, int epfd,int freeBytes)
{
    int ret=0;
    while(freeBytes>=SELLINGPACK && cb->currentSize>0)
    {

        struct myepoll* mep=(struct myepoll*)calloc(1,sizeof(struct myepoll));
        *mep=pop(cb);
        event->events=EPOLLOUT|EPOLLRDHUP;
        event->data.ptr=(void*)mep;
        if(epoll_ctl(epfd,EPOLL_CTL_ADD,mep->fd,event)==-1)
        {
            perror("error epoll_ctl134\n");
            exit(EXIT_FAILURE);
        }
        ret++;
        freeBytes-=SELLINGPACK;
    }
    if(ret>0) printf("Added %d clients from queue\n",ret);
    return ret;
}

char* getTS()
{
    char* ret=(char*)calloc(30,sizeof(char));
    struct timespec ts;
    if(clock_gettime(CLOCK_REALTIME,&ts)==-1)
    {
        perror("Error get TS\n");
        exit(EXIT_FAILURE);
    }
    sprintf(ret,"%lld.%.9ld",(long long)ts.tv_sec,ts.tv_nsec);
    return ret;
}

void readAndWaste(int pipefd,int size)
{
    char* buff=(char*)calloc(size,sizeof(char));
    if(read(pipefd,buff,size)==-1)
    {
        perror("Error read doczytanie\n");
        exit(EXIT_FAILURE);
    }
}









//circularBuffer functions

struct circularBuffer* create(int size)
{
    struct circularBuffer* buffer = (struct circularBuffer*)calloc(1,sizeof(struct circularBuffer));
    buffer->size = size;
    buffer->buffer = (struct myepoll*)calloc(size,sizeof(struct myepoll));
    buffer->currentSize = 0;
    buffer->first = 0;
    buffer->last = 0;
    return buffer;
}
void destroy(struct circularBuffer* buffer)
{
    free(buffer->buffer);
    free(buffer);
}
int add(struct circularBuffer* buffer, struct myepoll element)
{
    if (buffer->currentSize == buffer->size)
    {
        perror("No more place in a buffer!\n");
        return -1;
    }
    buffer->buffer[buffer->last] = element;
    buffer->last = (buffer->last+1)%(buffer->size);
    buffer->currentSize++;
    return 0;
}
struct myepoll pop(struct circularBuffer* buffer)
{
    struct myepoll temp = buffer->buffer[buffer->first];
    buffer->first = (buffer->first+1)%buffer->size;
    buffer->currentSize--;

    return temp;
}
int getCurrentSize(struct circularBuffer* buffer)
{
    return buffer->currentSize;
}