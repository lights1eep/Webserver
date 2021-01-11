#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <sys/prctl.h>

#define VERSION 23
#define BUFSIZE 8096
#define ERROR 42
#define LOG 44
#define FORBIDDEN 403
#define NOTFOUND 404
#define THREADNUM 300

#ifndef SIGCLD
#define SIGCLD SIGCHLD
#endif

struct
{
    char *ext;
    char *filetype;
} extensions[] =
{
    {"gif",  "image/gif"},
    {"jpg",  "image/jpg"},
    {"jpeg", "image/jpeg"},
    {"png",  "image/png"},
    {"ico",  "image/ico"},
    {"zip",  "image/zip"},
    {"gz",   "image/gz"},
    {"tar",  "image/tar"},
    {"htm",  "text/html"},
    {"html", "text/html"},
    {0,      0}
};

typedef struct//线程参数
{
    int hit;
    int fd;
} webparam;



typedef struct staconv//任务队列状态
{
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int status;
} staconv;


typedef struct task//任务
{
    struct task* next;
    void (*function)(void* arg);
    void *arg;
} task;


typedef struct taskqueue//任务队列
{
    pthread_mutex_t mutex;
    task *front;
    task *rear;
    staconv *has_jobs;
    int len;
} taskqueue;

typedef struct thread//线程
{
    int id;
    pthread_t pthread;
    struct threadpool *pool;
} thread;

typedef struct threadpool//线程池
{
    thread **threads;
    volatile int num_threads;
    volatile int num_working;
    pthread_mutex_t thcount_lock;
    pthread_cond_t threads_all_idle;
    taskqueue queue;
    volatile bool is_alive;
} threadpool;


struct threadpool* initThreadPool(int num_threads);
void init_taskqueue(taskqueue *queue);
void addTasktothreadPool(threadpool *pool,task *curtask);
void push_taskqueue(taskqueue *queue,task *curtask);
void waitThreadPool(threadpool* pool);
void destroy_taskqueue(taskqueue *queue);
int getNumofThreadWorking(threadpool *pool);
int create_thread(struct threadpool* pool,struct thread* pthread,int id);
void *thread_do(struct thread* pthread);
struct task* take_taskqueue(taskqueue *queue);
unsigned long get_file_size(const char *path);
void logger(int type,char *s1,char *s2,int socket_fd);
void *web(void *data);


struct threadpool* initThreadPool(int num_threads)//初始化线程池
{
    threadpool *pool;
    pool=(threadpool*)malloc(sizeof(struct threadpool));
    pool->num_threads=0;
    pool->num_working=0;
    pthread_mutex_init(&(pool->thcount_lock),NULL);
    pthread_cond_init(&(pool->threads_all_idle),NULL);
    pool->is_alive=true;

    init_taskqueue(&(pool->queue));

    pool->threads=(struct thread **)malloc(num_threads*sizeof(struct thread*));
    int i;
    for(i=0; i<num_threads; i++)
    {
        create_thread(pool,pool->threads[i],i);
    }
    while(pool->num_threads!=num_threads) {}
    return pool;
}

void init_taskqueue(taskqueue *queue)//初始化任务队列
{
    queue->front=NULL;
    queue->rear=NULL;
    queue->has_jobs=(staconv*)malloc(sizeof(struct staconv));
    pthread_mutex_init(&(queue->mutex),NULL);
    pthread_mutex_init(&(queue->has_jobs->mutex),NULL);
    pthread_cond_init(&(queue->has_jobs->cond),NULL);
    queue->has_jobs->status=false;
    queue->len=0;
}

void addTasktothreadPool(threadpool *pool,task *curtask)//任务队列放任务
{
    push_taskqueue(&(pool->queue),curtask);
}

void push_taskqueue(taskqueue *queue,task *curtask)//放任务
{
    pthread_mutex_lock(&(queue->mutex));
    if(queue->len==0)//任务队列为空
    {
        queue->front=curtask;
        queue->rear=curtask;
    }
    else//任务队列不空
    {
        queue->rear->next=curtask;
        queue->rear=curtask;
    }
    if(queue->len==0)//任务队列由空变为不空，修改状态
    {
        pthread_mutex_lock(&(queue->has_jobs->mutex));
        queue->has_jobs->status=true;
        pthread_cond_signal(&(queue->has_jobs->cond));
        pthread_mutex_unlock(&(queue->has_jobs->mutex));
    }
    queue->len++;//任务队列长度加一
    pthread_mutex_unlock(&(queue->mutex));
}
void waitThreadPool(threadpool* pool)//等待任务队列为空且无线程执行任务
{
    pthread_mutex_lock(&(pool->thcount_lock));
    while(pool->queue.len||pool->num_working)
    {
        pthread_cond_wait(&(pool->threads_all_idle),&(pool->thcount_lock));
    }
    pthread_mutex_unlock(&(pool->thcount_lock));
}

void destroyThreadPool(threadpool *pool)
{
    waitThreadPool(pool);//等待任务队列为空且无线程执行任务
	pool->is_alive=false;
	pthread_cond_broadcast(&(pool->queue.has_jobs->cond));    //释放所有挂起线程
	while(pool->num_threads!=0){}      //等待所有线程退出
	destroy_taskqueue(&(pool->queue));    //销毁任务队列
	pthread_mutex_destroy(&(pool->thcount_lock));     //销毁互斥锁
	pthread_cond_destroy(&(pool->threads_all_idle));    //销毁条件变量
	free(pool->threads);         //释放相应空间
	free(pool);

}

void destroy_taskqueue(taskqueue *queue)//销毁任务队列
{
    task* cur=queue->front;
    task* next;
    while(cur)
    {
        next=cur->next;
        free(cur);
        queue->len--;
        cur=next;
    }
    queue->front=NULL;
    queue->rear=NULL;
    pthread_mutex_destroy(&(queue->mutex));
    pthread_mutex_destroy(&(queue->has_jobs->mutex));
    pthread_cond_destroy(&(queue->has_jobs->cond));
    free(queue->has_jobs);
}
int getNumofThreadWorking(threadpool *pool)//获得任务队列长度
{
    return pool->num_working;
}

int create_thread(struct threadpool* pool,struct thread* pthread,int id)//创建进程
{
    pthread=(struct thread*)malloc(sizeof(struct thread));
    if(pthread==NULL)
    {
        perror("creat_thread(): Could not allocate memory for thread\n");
        return -1;
    }
    pthread->pool=pool;
    pthread->id=id;
    pthread_create(&(pthread->pthread),NULL,(void *)thread_do,pthread);
    pthread_detach(pthread->pthread);
    return 0;
}

void *thread_do(struct thread* pthread)//线程执行函数
{
	char thread_name[128]={0};
	sprintf(thread_name,"thread-poop-%d",pthread->id);

	prctl(PR_SET_NAME,thread_name);

	threadpool* pool=pthread->pool;

	pthread_mutex_lock(&(pool->thcount_lock));//修改线程池线程数量
	pool->num_threads++;
	pthread_mutex_unlock(&(pool->thcount_lock));

	while(pool->is_alive)
	{
		pthread_mutex_lock(&(pool->queue.has_jobs->mutex));//如果任务队列为空，挂起
		while(!pool->queue.has_jobs->status)
		{
 			pthread_cond_wait(&(pool->queue.has_jobs->cond),&(pool->queue.has_jobs->mutex));
		}
		pthread_mutex_unlock(&(pool->queue.has_jobs->mutex));


		if(pool->is_alive)
		{
			void(*func)(void*);
			void * arg;

			pthread_mutex_lock(&(pool->thcount_lock));//修改线程池工作线程数量
	 		pool->num_working++;
			pthread_mutex_unlock(&(pool->thcount_lock));

			task* curtask=take_taskqueue(&(pool->queue));//从任务队列获得任务
			if(curtask)//执行任务
			{
				func=curtask->function;
				arg=curtask->arg;
				func(arg);
				free(curtask);
			}
			pthread_mutex_lock(&(pool->thcount_lock));//任务执行完，修改工作线程数
			pool->num_working--;
			if(getNumofThreadWorking(pool)==0)//如果工作线程为0，释放
			{
				pthread_cond_broadcast(&(pool->threads_all_idle));
			}
			pthread_mutex_unlock(&(pool->thcount_lock));
		}
	}
	pthread_mutex_lock(&(pool->thcount_lock));//线程退出，修改线程数
	pool->num_threads--;
	pthread_mutex_unlock(&(pool->thcount_lock));
	return NULL;
}

struct task* take_taskqueue(taskqueue *queue)//从任务队列取任务
{
	task *curtask;
	pthread_mutex_lock(&(queue->mutex));
	curtask=queue->front;//从任务队列头去任务
	queue->len--;//任务队列长度减一
	if(queue->len==0)//取完任务后队列为空，则修改任务队列状态为false
	{
		queue->front=NULL;
		queue->rear=NULL;
		pthread_mutex_lock(&(queue->has_jobs->mutex));
		queue->has_jobs->status=false;
		pthread_mutex_unlock(&(queue->has_jobs->mutex));
	}
	else//不空，则将头指针指向下一个任务
	{
		queue->front=queue->front->next;
	}
	pthread_mutex_unlock(&(queue->mutex));
	return curtask;
}


unsigned long get_file_size(const char *path)
{
    unsigned long filesize=-1;
    struct stat statbuff;
    if(stat(path,&statbuff)<0)
    {
        return filesize;
    }
    else
    {
        filesize=statbuff.st_size;
    }
    return filesize;
}



void logger(int type,char *s1,char *s2,int socket_fd)
{
    int fd;
    char logbuffer[BUFSIZE * 2];
    /*根据消息类型，将消息放入 logbuffer 缓存，或直接将消息通过 socket 通道返回给客户端*/
    switch (type)
    {
    case ERROR:
        (void) sprintf(logbuffer, "ERROR: %s:%s Errno=%d exiting pid=%d", s1, s2, errno, getpid());
        break;
    case FORBIDDEN:
        (void) write(socket_fd,
                     "HTTP/1.1 403 Forbidden\n"
                     "Content-Length: 185\n"
                     "Connection:close\n"
                     "Content-Type: text/html\n\n"
                     "<html><head>\n"
                     "<title>403 Forbidden</title>\n"
                     "</head><body>\n"
                     "<h1>Forbidden</h1>\n"
                     "The requested URL, file type or operationis not allowed on this simple static file webserver.\n"
                     "</body></html>\n",
                     271);
        (void) sprintf(logbuffer, "FORBIDDEN: %s:%s", s1, s2);
        break;
    case NOTFOUND:
        (void) write(socket_fd,
                     "HTTP/1.1 404 Not Found\n"
                     "Content-Length:136\n"
                     "Connection: close\n"
                     "Content-Type: text/html\n\n"
                     "<html><head>\n"
                     "<title>404 Not Found</title>\n"
                     "</head><body>\n"
                     "<h1>Not Found</h1>\n"
                     "The requested URL was not found on this server.\n"
                     "</body></html>\n",
                     224);
        (void) sprintf(logbuffer, "NOT FOUND: %s:%s", s1, s2);
        break;
    case LOG:
        (void) sprintf(logbuffer, " INFO: %s:%s:%d", s1, s2, socket_fd);
        break;
    }
    /* 将 logbuffer 缓存中的消息存入 webserver.log 文件*/
    if ((fd = open("nweb.log", O_CREAT | O_WRONLY | O_APPEND, 0644)) >= 0)
    {
        (void) write(fd, logbuffer, strlen(logbuffer));
        (void) write(fd, "\n", 1);
        (void) close(fd);
    }
}

void *web(void *data)
{
    int fd,hit,fd1;
    int j, file_fd, buflen;
    long i, ret, len;
    char *fstr;
    char buffer[BUFSIZE + 1]; /* 设置静态缓冲区 */
    webparam *param=(webparam*) data;
    fd=param->fd;
    hit=param->hit;



    ret = read(fd, buffer, BUFSIZE);
    /* 从连接通道中读取客户端的请求消息 */

    if (ret == 0 || ret == -1)   //如果读取客户端消息失败，则向客户端发送 HTTP 失败响应信息
    {
        logger(FORBIDDEN, "failed to read browser request", "", fd);
    }
    else
    {
        if (ret > 0 && ret < BUFSIZE)
            /* 设置有效字符串，即将字符串尾部表示为 0 */
            buffer[ret] = 0;
        else buffer[0] = 0;
        for (i = 0; i < ret; i++) /* 移除消息字符串中的“CF”和“LF”字符*/
            if (buffer[i] == '\r' || buffer[i] == '\n')
                buffer[i] = '*';
        logger(LOG, "request", buffer, hit);
        /*判断客户端 HTTP 请求消息是否为 GET 类型，如果不是则给出相应的响应消息*/
        if (strncmp(buffer, "GET ", 4) && strncmp(buffer, "get ", 4))
        {
            logger(FORBIDDEN, "Only simple GET operation supported", buffer, fd);
        }

        for (i = 4; i < BUFSIZE; i++)   /* null terminate after the second space to ignore extra stuff */
        {
            if (buffer[i] == ' ')   /* string is "GET URL " +lots of other stuff */
            {
                buffer[i] = 0;
                break;
            }
        }
        for (j = 0; j < i - 1; j++)
        {
            /* 在消息中检测路径，不允许路径中出现“.” */

            if (buffer[j] == '.' && buffer[j + 1] == '.')
            {
                logger(FORBIDDEN, "Parent directory (..) path names not supported", buffer, fd);
            }

        }
        if (!strncmp(&buffer[0], "GET /\0", 6) || !strncmp(&buffer[0], "get /\0", 6))
            /* 如果请求消息中没有包含有效的文件名，则使用默认的文件名 index.html */
            (void) strcpy(buffer, "GET /index.html");
        /* 根据预定义在 extensions 中的文件类型，检查请求的文件类型是否本服务器支持 */
        buflen = strlen(buffer);
        fstr = (char *) 0;
        for (i = 0; extensions[i].ext != 0; i++)
        {
            len = strlen(extensions[i].ext);
            if (!strncmp(&buffer[buflen - len], extensions[i].ext, len))
            {
                fstr = extensions[i].filetype;
                break;
            }
        }
        if (fstr == 0)
        {
            logger(FORBIDDEN, "file extension type not supported", buffer, fd);
        }
        if ((file_fd = open(&buffer[5], O_RDONLY)) == -1)   /* 打开指定的文件名*/
        {
            logger(NOTFOUND, "failed to open file", &buffer[5], fd);
        }

        logger(LOG, "SEND", &buffer[5], hit);

        len = (long) lseek(file_fd, (off_t) 0, SEEK_END); /* 通过 lseek 获取文件长度*/
        (void) lseek(file_fd, (off_t) 0, SEEK_SET); /* 将文件指针移到文件首位置*/

        (void) sprintf(buffer,
                       "HTTP/1.1 200 OK\nServer:nweb/%d.0\nContent-Length:%ld\nConnection:close\nContent-Type: %s\n\n",
                       VERSION, len, fstr); /* Header + a blank line */
        logger(LOG, "Header", buffer, hit);

        (void) write(fd, buffer, strlen(buffer));
        /* 不停地从文件里读取文件内容，并通过 socket 通道向客户端返回文件内容*/
        while ((ret = read(file_fd, buffer, BUFSIZE)) > 0)
        {
            (void) write(fd, buffer, ret);
        }

        usleep(10000);/*sleep 的作用是防止消息未发出，已经将此 socket 通道关闭*/
        close(file_fd);
    }
    close(fd);
    free(param);
    return 0;
}



int main(int argc, char **argv)
{
    int i, port, listenfd, socketfd, hit;
    socklen_t length;
    static struct sockaddr_in cli_addr; /* static = initialised to zeros */
    static struct sockaddr_in serv_addr; /* static = initialised to zeros */


    /*解析命令参数*/
    if (argc < 3 || argc > 3 || !strcmp(argv[1], "-?"))
    {
        (void) printf("hint: nweb Port-Number Top-Directory\t\tversion %d\n\n"
                      "\tnweb is a small and very safe mini web server\n"
                      "\tnweb only servers out file/web pages with extensions named below\n"
                      "\t and only from the named directory or its sub-directories.\n"
                      "\tThere is no fancy features = safe and secure.\n\n"
                      "\tExample:webserver 8181 /home/nwebdir &\n\n"
                      "\tOnly Supports:", VERSION);
        for (i = 0; extensions[i].ext != 0; i++)
            (void) printf(" %s", extensions[i].ext);
        (void) printf("\n\tNot Supported: URLs including \"..\", Java, Javascript, CGI\n"
                      "\tNot Supported: directories / /etc /bin /lib /tmp /usr /dev /sbin \n"
                      "\tNo warranty given or implied\n\tNigel Griffiths nag@uk.ibm.com\n");
        exit(0);
    }
    if (!strncmp(argv[2], "/", 2) || !strncmp(argv[2], "/etc", 5) ||
            !strncmp(argv[2], "/bin", 5) || !strncmp(argv[2], "/lib", 5) ||
            !strncmp(argv[2], "/tmp", 5) || !strncmp(argv[2], "/usr", 5) ||
            !strncmp(argv[2], "/dev", 5) || !strncmp(argv[2], "/sbin", 6))
    {
        (void) printf("ERROR: Bad top directory %s, see nweb -?\n", argv[2]);
        exit(3);
    }
    if (chdir(argv[2]) == -1)
    {
        (void) printf("ERROR: Can't Change to directory %s\n", argv[2]);
        exit(4);
    }
    /*if(fork()!=0)
    {
    	return 0;
    }
    (void)signal(SIGCLD,SIG_IGN);
    (void)signal(SIGHUP,SIG_IGN);
    for(i=0;i<32;i++)
    {
       	(void)close(i);
    }
    (void)setpgrp();*/

    logger(LOG,"nweb starting",argv[1],getpid());
    /* 建立服务端侦听 socket*/
    if ((listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        logger(ERROR, "system call", "socket", 0);
    port = atoi(argv[1]);
    if (port < 0 || port > 60000)
        logger(ERROR, "Invalid port number (try 1->60000)", argv[1], 0);

    /*pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
    pthread_t pth;*/
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(port);
    if (bind(listenfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0)
        logger(ERROR, "system call", "bind", 0);
    if (listen(listenfd, 64) < 0)
        logger(ERROR, "system call", "listen", 0);

    threadpool *pool=initThreadPool(THREADNUM);

    for (hit = 1;; hit++)
    {
        length = sizeof(cli_addr);
        if ((socketfd = accept(listenfd, (struct sockaddr *) &cli_addr, &length)) < 0)
            logger(ERROR, "system call", "accept", 0);

        webparam *param=malloc(sizeof(webparam));
        param->hit=hit;
        param->fd=socketfd;
        task* curtask=(task*)malloc(sizeof(struct task*));
        curtask->next=NULL;
        curtask->function=(void*)web;
        curtask->arg=(void*)param;
        addTasktothreadPool(pool,curtask);


    }
    destroyThreadPool(pool);
    /*if(pthread_create(&pth,&attr,&web,(void*)param)<0)
          	 logger(ERROR,"system call","pthread_create",0);*/
}
