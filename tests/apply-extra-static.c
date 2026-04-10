#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

static const char msg[] = "noruntime-extra-data\n";

int main(void) {
    mkdir("/app/extra", 0755);
    int fd = open("/app/extra/ran", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return 1;
    write(fd, msg, sizeof(msg) - 1);
    close(fd);
    return 0;
}
