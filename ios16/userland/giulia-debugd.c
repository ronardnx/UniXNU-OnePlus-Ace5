/* Root-only USB <-> PTY bridge. No TCP listener and no command emulation.
 * Execute a real, separately staged Darwin ARM64 shell with its normal tools.
 */
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <util.h>
#include "../giulia_debug_abi.h"

static volatile sig_atomic_t stopping;
static void stop(int sig) { (void)sig;stopping=1; }
static int nonblock(int fd) {
    int flags=fcntl(fd,F_GETFL);
    return flags<0?-1:fcntl(fd,F_SETFL,flags|O_NONBLOCK);
}
static void terminate_shell(pid_t pid,int master)
{
    pid_t foreground=tcgetpgrp(master);
    if(foreground>1)kill(-foreground,SIGHUP);
    kill(-pid,SIGHUP);close(master);
    for(unsigned i=0;i<50;i++) {
        pid_t result=waitpid(pid,NULL,WNOHANG);
        if(result==pid || (result<0 && errno==ECHILD))return;
        usleep(10000);
    }
    if(foreground>1)kill(-foreground,SIGKILL);
    kill(-pid,SIGKILL);kill(pid,SIGKILL);
    while(waitpid(pid,NULL,0)<0 && errno==EINTR) {}
}
static int pump_write(int fd,unsigned char *data,size_t *length)
{
    if(!*length)return 0;
    ssize_t n=write(fd,data,*length);
    if(n<0) return (errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR)?0:-1;
    memmove(data,data+n,*length-(size_t)n);*length-=(size_t)n;
    return 0;
}
static void session(int device,const struct giulia_shell_state *initial,const char *shell)
{
    int master=-1;
    struct winsize size={.ws_row=(unsigned short)initial->rows,.ws_col=(unsigned short)initial->columns};
    pid_t child=forkpty(&master,NULL,NULL,&size);
    if(child<0) { perror("forkpty");ioctl(device,GIULIA_SHELL_END);return; }
    if(!child) {
        close(device);
        signal(SIGPIPE,SIG_DFL);signal(SIGTERM,SIG_DFL);signal(SIGINT,SIG_DFL);
        setenv("TERM","xterm-256color",1);
        setenv("PATH","/usr/local/giulia/bin:/usr/bin:/bin:/usr/sbin:/sbin",1);
        setenv("HOME","/var/root",1);
        if(chdir("/var/root")) (void)chdir("/");
        execl(shell,shell,"-i",(char *)NULL);
        dprintf(STDERR_FILENO,"giulia-debugd: exec %s failed: %s\r\n",shell,strerror(errno));
        _exit(127);
    }
    if(nonblock(master)<0) {terminate_shell(child,master);ioctl(device,GIULIA_SHELL_END);return;}
    unsigned char input[4096],output[4096];size_t ni=0,no=0;
    int eof=0;
    while(!stopping) {
        struct giulia_shell_state state;
        if(ioctl(device,GIULIA_SHELL_STATE,&state)<0 || !state.active || state.generation!=initial->generation)break;
        if(ni<sizeof(input) && !eof) {
            ssize_t n=read(device,input+ni,sizeof(input)-ni);
            if(n>0)ni+=(size_t)n;
            else if(n<0 && errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR)break;
        }
        if(pump_write(master,input,&ni)<0)eof=1;
        if(!eof && no<sizeof(output)) {
            ssize_t n=read(master,output+no,sizeof(output)-no);
            if(n>0)no+=(size_t)n;
            else if(!n || (n<0 && errno==EIO))eof=1;
            else if(n<0 && errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR)break;
        }
        if(pump_write(device,output,&no)<0)break;
        if(eof && !no)break;
        usleep(10000);
    }
    terminate_shell(child,master);
    (void)ioctl(device,GIULIA_SHELL_END);
}
int main(int argc,char **argv)
{
    const char *shell=argc==2?argv[1]:"/usr/local/giulia/bin/sh";
    if(argc>2 || shell[0]!='/' || geteuid()!=0) {fprintf(stderr,"usage (root): giulia-debugd /absolute/shell\n");return 2;}
    if(access(shell,X_OK)) {perror(shell);return 1;}
    signal(SIGTERM,stop);signal(SIGINT,stop);signal(SIGPIPE,SIG_IGN);
    int device=-1;
    while(!stopping && device<0) {
        device=open("/dev/giulia-debug",O_RDWR|O_NONBLOCK|O_CLOEXEC);
        if(device<0) {
            if(errno!=ENOENT && errno!=ENXIO) {perror("/dev/giulia-debug");return 1;}
            sleep(1);
        }
    }
    if(device<0)return 0;
    while(!stopping) {
        struct giulia_shell_state state;
        if(ioctl(device,GIULIA_SHELL_STATE,&state)<0) {perror("shell state");break;}
        if(state.active)session(device,&state,shell);
        else usleep(20000);
    }
    close(device);return 0;
}
