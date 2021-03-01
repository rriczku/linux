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
#include <net/if.h>

#define STORAGE 30720
#define BLOCKSIZE 13312

struct parameters{
    int c; //pojemnosc maagazynu
    float p; //tempo konsumpcji 4435/s danych
    float d; //tempo degradacji materialu
    struct sockaddr_in a;
};
struct onexitparameters{
    struct timespec* one;
    struct timespec* two;
    char* adress;
    int port;
};

struct parameters parseParams(int argc, char** argv);
float parseFloat(char* s);
int parseInt(char* s);
void adressParsing(char* s, struct parameters* p);
int connectToServer(struct sockaddr_in* addr);
void doJob(int socket,struct parameters p,int* freeSpace);
void printRaport(int socket);
void printPackRaport(int status, void* data);
void getAddress(int socket,char** address, int* port);
char* getTS();
struct timespec getTimeToSleep(float p);
struct timespec getTSRealtime();
struct timespec getTSMonotonic();
struct timespec getLatency(struct timespec t1,struct timespec t2);
int degradation(int seconds,int nanoseconds,float d);

int main(int argc, char* argv[])
{

    struct parameters p=parseParams(argc,argv);
    int freeSpace=p.c*STORAGE;
    int socket=connectToServer(&p.a);
    doJob(socket,p,&freeSpace);
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
    while((opt=getopt(argc,argv,"c:p:d:"))!=-1)
    {
        switch(opt)
        {
            case 'p':
                ret.p=parseFloat(optarg)*4435;
                break;
            case 'c':
                ret.c=parseInt(optarg);
                break;
            case 'd':
                ret.d=parseFloat(optarg)*819;
                break;
            default:
                fprintf(stderr,"Wrong parameter!\n");
                exit(1);
                break;
        }
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
int connectToServer(struct sockaddr_in* addr)
{
    int sDesc=socket(AF_INET,SOCK_STREAM,0);
    if(sDesc==-1)
    {
        perror("Error socket\n");
        exit(EXIT_FAILURE);
    }
    addr->sin_family=AF_INET;

    if(connect(sDesc,(struct sockaddr*)addr,sizeof(struct sockaddr))<0)
    {
        perror("Error connect!\n");
        exit(EXIT_FAILURE);
    }
    return sDesc;
}
void doJob(int socket,struct parameters p,int* freeSpace)
{
    int packcounter=0;
    struct timespec connectionTime=getTSMonotonic();
    struct timespec firstPackTime, connectionLostTime;
    struct onexitparameters* oep=(struct onexitparameters*)calloc(1,sizeof(struct onexitparameters));
    oep->one=(struct timespec*)calloc(1,sizeof(struct timespec));
    oep->two=(struct timespec*)calloc(1,sizeof(struct timespec));
    oep->adress=(char*)calloc(15,sizeof(char));
    struct timespec ts=getTimeToSleep(p.p);
    for(;;)
    {
        char* buff=(char*)calloc((BLOCKSIZE/4),sizeof(char));
        int bytesRead=recv(socket,buff,BLOCKSIZE/4,0);
        if(bytesRead==0)
        {
            connectionLostTime=getTSMonotonic();
            *(oep->one)=getLatency(connectionTime,firstPackTime);
            *(oep->two)=getLatency(firstPackTime,connectionLostTime);
            getAddress(socket,&(oep->adress),&(oep->port));
            on_exit(printPackRaport,(void*)oep);
            int degradated=degradation(oep->two->tv_sec,oep->two->tv_nsec,p.d); // degradacja w trakcie czytania
            if(*freeSpace!=STORAGE) {
                struct timespec lat=getLatency(connectionTime, firstPackTime);
                degradated += degradation(lat.tv_sec,lat.tv_nsec,p.d);
            }
            if((*freeSpace+degradated)>p.c*STORAGE) *freeSpace=p.c*STORAGE;
            else *freeSpace=*freeSpace+degradated;
            if(*freeSpace>=BLOCKSIZE)
            {
                close(socket);
                socket=connectToServer(&p.a);
                doJob(socket,p,freeSpace);
                break;
            }else{
                printRaport(socket);
                close(socket);
                exit(EXIT_SUCCESS);
            }
        }
        else if(bytesRead==-1)
        {
            perror("Error connection with server\n");
            exit(EXIT_FAILURE);
        }
        *freeSpace-=bytesRead;
        if(packcounter==0)
        {
            firstPackTime=getTSMonotonic();
        }
        packcounter++;
        nanosleep(&ts,NULL);
    }
}

void printRaport(int socket)
{
    char* ts=getTS();
    pid_t pid=getpid();
    char* ip=(char*)calloc(15,sizeof(char));
    int port=0;
    getAddress(socket,&ip,&port);
    fprintf(stderr,"END! TS: %s PID: %d\n",ts,pid);
}
char* getTS()
{
    char* ret=(char*)calloc(30,sizeof(char));
    struct timespec ts=getTSRealtime();
    sprintf(ret,"%lld.%.9ld",(long long)ts.tv_sec,ts.tv_nsec);
    return ret;
}
void getAddress(int socket,char** address, int* port)
{
    struct sockaddr_in addr;
    unsigned int addrlen=sizeof(addr);
    if(getsockname(socket,&addr,&addrlen)==-1)
    {
        perror("Error getpeername\n");
        exit(EXIT_FAILURE);
    }
    *port=addr.sin_port;
    strcpy(*address,inet_ntoa(addr.sin_addr));
}
void printPackRaport(int status, void* data)
{
    struct onexitparameters* times=(struct onexitparameters*)data;
    fprintf(stderr,"%s %d T connection and first part : %ld s %ld ns T first part and connection close : %ld s %ld ns\n",times->adress,times->port,times->one->tv_sec,times->one->tv_nsec,times->two->tv_sec,times->two->tv_nsec);
    free(times);
}
struct timespec getTSRealtime()
{
    struct timespec ts;
    if(clock_gettime(CLOCK_REALTIME,&ts)==-1)
    {
        perror("Error clock_gettime\n");
        exit(EXIT_FAILURE);
    }
    return ts;
}
struct timespec getTSMonotonic()
{
    struct timespec ts;
    if(clock_gettime(CLOCK_MONOTONIC,&ts)==-1)
    {
        perror("Error clock_gettime\n");
        exit(EXIT_FAILURE);
    }
    return ts;
}
struct timespec getTimeToSleep(float p)
{
    struct timespec ret={0};
    float temp=(BLOCKSIZE)/(4*p);
    long ns=temp*1000000000;
    ret.tv_sec=ns/1000000000;
    ret.tv_nsec=ns-ret.tv_sec*1000000000;
    return ret;
}
int degradation(int seconds,int nanoseconds,float d)
{
    int ret=0; // degradated bytes
    float s=seconds+nanoseconds/1000000000;
    ret=s*d;
    return ret;
}
struct timespec getLatency(struct timespec t1,struct timespec t2)
{
    struct timespec ret;
    long ns1=t1.tv_nsec+t1.tv_sec*1000000000;
    long ns2=t2.tv_nsec+t2.tv_sec*1000000000;
    long sub=ns2-ns1;
    ret.tv_sec=sub/1000000000;
    ret.tv_nsec=sub-ret.tv_sec*1000000000;
    return ret;
}