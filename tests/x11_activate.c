#include <X11/Xlib.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    XEvent event;
    Display *display;
    Window root;
    Window window;
    Atom active_window;
    char *end = NULL;

    if (argc != 2) {
        fprintf(stderr, "usage: %s WINDOW_ID\n", argv[0]);
        return 2;
    }
    errno = 0;
    window = (Window)strtoul(argv[1], &end, 0);
    if (errno || end == argv[1] || *end) return 2;

    display = XOpenDisplay(NULL);
    if (!display) return 1;
    root = DefaultRootWindow(display);
    active_window = XInternAtom(display, "_NET_ACTIVE_WINDOW", False);
    memset(&event, 0, sizeof(event));
    event.xclient.type = ClientMessage;
    event.xclient.window = window;
    event.xclient.message_type = active_window;
    event.xclient.format = 32;
    event.xclient.data.l[0] = 2;
    event.xclient.data.l[1] = CurrentTime;
    XSendEvent(display, root, False, SubstructureRedirectMask | SubstructureNotifyMask, &event);
    XFlush(display);
    XCloseDisplay(display);
    return 0;
}
