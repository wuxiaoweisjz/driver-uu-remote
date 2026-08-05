#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int emit(int fd, unsigned short type, unsigned short code, int value)
{
    struct input_event event;

    memset(&event, 0, sizeof(event));
    event.type = type;
    event.code = code;
    event.value = value;
    return write(fd, &event, sizeof(event)) == (ssize_t)sizeof(event);
}

int main(void)
{
    struct uinput_setup setup;
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);

    if (fd < 0) {
        perror("open /dev/uinput");
        return 1;
    }
    if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 || ioctl(fd, UI_SET_KEYBIT, KEY_ESC) < 0) {
        perror("configure uinput");
        close(fd);
        return 1;
    }

    memset(&setup, 0, sizeof(setup));
    setup.id.bustype = BUS_USB;
    setup.id.vendor = 0x1209;
    setup.id.product = 0x0002;
    strcpy(setup.name, "UU Remote test keyboard");
    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
        perror("create uinput device");
        close(fd);
        return 1;
    }

    usleep(250000);
    emit(fd, EV_KEY, KEY_ESC, 1);
    emit(fd, EV_SYN, SYN_REPORT, 0);
    emit(fd, EV_KEY, KEY_ESC, 0);
    emit(fd, EV_SYN, SYN_REPORT, 0);
    usleep(100000);
    ioctl(fd, UI_DEV_DESTROY);
    close(fd);
    return 0;
}
