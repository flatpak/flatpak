#include <err.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

static const char msg[] = "noruntime-extra-data\n";

int main(void) {
    int dfd = -1;
    int exe_fd = -1;

    mkdir("/app/extra", 0755);
    int fd = open("/app/extra/ran", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return 1;
    ssize_t ret = write(fd, msg, sizeof(msg) - 1);
    (void)ret;
    close(fd);

    dfd = open ("/app/extra", O_CLOEXEC | O_DIRECTORY | O_PATH | O_NOFOLLOW);

    if (dfd < 0)
        err (1, "open /app/extra");

    exe_fd = openat (dfd, "exe", O_CLOEXEC | O_CREAT | O_EXCL | O_RDWR, 02744);

    if (exe_fd < 0)
        err (1, "openat /app/extra/exe");

    if (fchmod (exe_fd, 06744) < 0)
        err (1, "fchmod /app/extra/exe");

    if (close (exe_fd) < 0)
        err (1, "close /app/extra/exe");

    exe_fd = -1;

    close (dfd);
    dfd = -1;

    return 0;
}
