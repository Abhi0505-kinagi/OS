#include<stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <string.h>
#include "monitor_ioctl.h"

int main() {
    int fd = open("/dev/container_monitor", O_RDWR);
    if(fd < 0) { perror("open"); return 1; }

    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    strcpy(req.container_id, "test");
    req.pid = getpid();          // monitor this process
    req.soft_limit_bytes = 10*1024*1024; // 10 MB
    req.hard_limit_bytes = 20*1024*1024; // 20 MB

    if(ioctl(fd, MONITOR_REGISTER, &req) < 0) {
        perror("ioctl register");
        return 1;
    }

    printf("Registered PID %d with monitor\n", req.pid);

    // Allocate memory to trigger soft/hard limits
    char *buf = NULL;
    size_t size = 0;
    while(1) {
        buf = malloc(1024*1024); // 1MB chunks
        if(!buf) break;
        size += 1024*1024;
        printf("Allocated %zu MB\n", size/(1024*1024));
        sleep(1);
    }

    close(fd);
    return 0;
}
