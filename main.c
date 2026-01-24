// CalWM-style minimal window manager
// - frame + titlebar + close button
// - click-to-focus + raise
// - simple taskbar
// - robust XError handler
// - windows fully removed on close (Unmap + Destroy)
// - WM only creates/destroys its own windows; never pokes dead clients
// - proper Expose handling during moves
// - normal left-click input (no weird middle+left combo)
// - Alt + RightClick + drag to resize from bottom-right

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BORDER_WIDTH   2
#define TITLE_HEIGHT   24
#define TASKBAR_HEIGHT 24
#define CLOSE_WIDTH    20
#define MIN_WIDTH      80
#define MIN_HEIGHT     60

typedef struct Client {
    Window win;       // client window
    Window frame;     // outer frame
    Window titlebar;  // titlebar window
    int x, y, w, h;
    char *name;
    struct Client *next;
} Client;

static Display *dpy;
static int screen;
static Window root;
static Window taskbar;
static Client *clients = NULL;

static unsigned long border_focus;
static unsigned long border_unfocus;
static unsigned long bg_color;
static unsigned long title_bg;
static unsigned long title_fg;
static unsigned long taskbar_bg;
static unsigned long taskbar_fg;

static Atom WM_DELETE_WINDOW;
static Atom WM_PROTOCOLS;
static Atom WM_NAME_ATOM;

/* ---------- Error handling ---------- */

static int xerror(Display *d, XErrorEvent *e) {
    (void)d;
    (void)e;
    // Window managers hit BadWindow all the time when clients die.
    // Swallow everything; stability > noise.
    return 0;
}

/* ---------- Client lookup helpers ---------- */

static Client *find_client_any(Window w) {
    for (Client *c = clients; c; c = c->next) {
        if (c->win == w || c->frame == w || c->titlebar == w)
            return c;
    }
    return NULL;
}

static Client *find_client_by_win(Window w) {
    for (Client *c = clients; c; c = c->next) {
        if (c->win == w)
            return c;
    }
    return NULL;
}

static void add_client(Client *c) {
    c->next = clients;
    clients = c;
}

/* ---------- Client metadata ---------- */

static int window_alive(Window w) {
    XWindowAttributes attr;
    return XGetWindowAttributes(dpy, w, &attr) != 0;
}

static void update_client_name(Client *c) {
    if (!c) return;
    if (!window_alive(c->win)) return;

    XTextProperty prop;
    if (XGetWMName(dpy, c->win, &prop) && prop.value && prop.nitems) {
        free(c->name);
        c->name = strndup((char *)prop.value, prop.nitems);
        XFree(prop.value);
    } else {
        if (!c->name) c->name = strdup("CalWM window");
    }
}

/* ---------- Focus and drawing ---------- */

static void focus_client(Client *c) {
    if (!c) return;
    if (!window_alive(c->win)) return;

    XRaiseWindow(dpy, c->frame);
    XSetInputFocus(dpy, c->win, RevertToPointerRoot, CurrentTime);

    for (Client *it = clients; it; it = it->next) {
        unsigned long col = (it == c) ? border_focus : border_unfocus;
        XSetWindowBorder(dpy, it->frame, col);
    }
}

static void draw_title(Client *c) {
    if (!c) return;
    GC gc = XCreateGC(dpy, c->titlebar, 0, NULL);

    XSetForeground(dpy, gc, title_bg);
    XFillRectangle(dpy, c->titlebar, gc, 0, 0, c->w, TITLE_HEIGHT);

    // close button
    XSetForeground(dpy, gc, border_unfocus);
    XFillRectangle(dpy, c->titlebar, gc,
                   c->w - CLOSE_WIDTH, 0, CLOSE_WIDTH, TITLE_HEIGHT);
    XSetForeground(dpy, gc, title_fg);
    XDrawString(dpy, c->titlebar, gc,
                c->w - CLOSE_WIDTH + 6, TITLE_HEIGHT - 8, "X", 1);

    update_client_name(c);
    if (c->name) {
        XSetForeground(dpy, gc, title_fg);
        XDrawString(dpy, c->titlebar, gc, 4, TITLE_HEIGHT - 8,
                    c->name, (int)strlen(c->name));
    }

    XFreeGC(dpy, gc);
}

static void draw_taskbar(void) {
    GC gc = XCreateGC(dpy, taskbar, 0, NULL);
    int sw = DisplayWidth(dpy, screen);

    XSetForeground(dpy, gc, taskbar_bg);
    XFillRectangle(dpy, taskbar, gc, 0, 0, sw, TASKBAR_HEIGHT);

    int count = 0;
    for (Client *c = clients; c; c = c->next) count++;
    if (count == 0) {
        XFreeGC(dpy, gc);
        return;
    }

    int slot = sw / count;
    int i = 0;

    for (Client *c = clients; c; c = c->next, i++) {
        int x = i * slot;
        XSetForeground(dpy, gc, border_unfocus);
        XDrawRectangle(dpy, taskbar, gc, x, 0, slot - 1, TASKBAR_HEIGHT - 1);

        update_client_name(c);
        const char *name = c->name ? c->name : "CalWM";
        XSetForeground(dpy, gc, taskbar_fg);
        XDrawString(dpy, taskbar, gc, x + 4, TASKBAR_HEIGHT - 8,
                    name, (int)strlen(name));
    }

    XFreeGC(dpy, gc);
}

/* ---------- Manage / unmanage ---------- */

static void unmanage(Client *c) {
    if (!c) return;

    // Remove from list first so nothing else can find it.
    if (clients == c) {
        clients = c->next;
    } else {
        Client *p = clients;
        while (p && p->next != c) p = p->next;
        if (p) p->next = c->next;
    }

    // Destroy only our own windows. Titlebar dies with frame.
    XDestroyWindow(dpy, c->frame);

    free(c->name);
    free(c);

    draw_taskbar();
}

static void manage(Window w) {
    XWindowAttributes attr;
    if (!XGetWindowAttributes(dpy, w, &attr) || attr.override_redirect)
        return;

    // Don't manage our own windows (taskbar, frames, titlebars).
    if (w == taskbar || find_client_any(w))
        return;

    Client *c = calloc(1, sizeof(Client));
    if (!c) return;

    c->win = w;
    c->x = attr.x;
    c->y = attr.y;
    c->w = attr.width;
    c->h = attr.height;

    int sw = DisplayWidth(dpy, screen);
    int sh = DisplayHeight(dpy, screen);

    if (c->y + c->h + TITLE_HEIGHT > sh - TASKBAR_HEIGHT)
        c->y = sh - TASKBAR_HEIGHT - (c->h + TITLE_HEIGHT);

    c->frame = XCreateSimpleWindow(
        dpy, root,
        c->x, c->y,
        c->w,
        c->h + TITLE_HEIGHT,
        BORDER_WIDTH,
        border_unfocus,
        bg_color
    );

    c->titlebar = XCreateSimpleWindow(
        dpy, c->frame,
        0, 0,
        c->w,
        TITLE_HEIGHT,
        0,
        border_unfocus,
        title_bg
    );

    XSelectInput(dpy, c->frame,
                 ExposureMask |
                 ButtonPressMask |
                 ButtonReleaseMask |
                 PointerMotionMask |
                 SubstructureNotifyMask);  // see child Unmap/Destroy

    XSelectInput(dpy, c->titlebar,
                 ExposureMask |
                 ButtonPressMask |
                 ButtonReleaseMask |
                 PointerMotionMask);

    // NORMAL input on client: no passive grab, just listen for ButtonPress.
    XSelectInput(dpy, c->win,
                 PropertyChangeMask |
                 EnterWindowMask |
                 LeaveWindowMask |
                 FocusChangeMask |
                 ButtonPressMask);

    // Reparent client into our frame
    XReparentWindow(dpy, c->win, c->frame, 0, TITLE_HEIGHT);

    XSetWMProtocols(dpy, c->win, &WM_DELETE_WINDOW, 1);

    XMapWindow(dpy, c->win);
    XMapWindow(dpy, c->titlebar);
    XMapWindow(dpy, c->frame);

    update_client_name(c);
    add_client(c);
    focus_client(c);
    draw_title(c);
    draw_taskbar();
}

/* ---------- Forward declarations for dispatcher ---------- */

static void handle_map_request(XEvent *e);
static void handle_destroy_notify(XEvent *e);
static void handle_unmap_notify(XEvent *e);
static void handle_configure_request(XEvent *e);
static void handle_client_message(XEvent *e);
static void handle_button_press(XEvent *e);
static void handle_expose(XEvent *e);
static void handle_property_notify(XEvent *e);

/* ---------- Event dispatcher (used in move/resize loops) ---------- */

static void dispatch_event(XEvent *e) {
    switch (e->type) {
    case MapRequest:
        handle_map_request(e);
        break;
    case DestroyNotify:
        handle_destroy_notify(e);
        break;
    case UnmapNotify:
        handle_unmap_notify(e);
        break;
    case ConfigureRequest:
        handle_configure_request(e);
        break;
    case ClientMessage:
        handle_client_message(e);
        break;
    case ButtonPress:
        handle_button_press(e);
        break;
    case Expose:
        handle_expose(e);
        break;
    case PropertyNotify:
        handle_property_notify(e);
        break;
    default:
        break;
    }
}

/* ---------- Alt+Right drag resize ---------- */

static void start_resize(Client *c, int start_x, int start_y) {
    int orig_w = c->w;
    int orig_h = c->h;

    XGrabPointer(dpy, root, False,
                 PointerMotionMask | ButtonReleaseMask,
                 GrabModeAsync, GrabModeAsync,
                 None, None, CurrentTime);

    for (;;) {
        XEvent ev;
        XNextEvent(dpy, &ev);

        if (ev.type == MotionNotify) {
            int dx = ev.xmotion.x_root - start_x;
            int dy = ev.xmotion.y_root - start_y;

            int new_w = orig_w + dx;
            int new_h = orig_h + dy;

            if (new_w < MIN_WIDTH)  new_w = MIN_WIDTH;
            if (new_h < MIN_HEIGHT) new_h = MIN_HEIGHT;

            c->w = new_w;
            c->h = new_h;

            XMoveResizeWindow(dpy, c->frame,
                              c->x, c->y,
                              c->w,
                              c->h + TITLE_HEIGHT);
            XMoveResizeWindow(dpy, c->win,
                              0, TITLE_HEIGHT,
                              c->w, c->h);
            XMoveResizeWindow(dpy, c->titlebar,
                              0, 0,
                              c->w, TITLE_HEIGHT);

            draw_title(c);
            draw_taskbar();
        } else if (ev.type == ButtonRelease) {
            break;
        } else {
            // Keep handling other events (Expose, etc.) during resize.
            dispatch_event(&ev);
        }
    }

    XUngrabPointer(dpy, CurrentTime);
}

/* ---------- Event handlers ---------- */

static void handle_map_request(XEvent *e) {
    XMapRequestEvent *ev = &e->xmaprequest;

    // Ignore our own windows.
    if (ev->window == taskbar || find_client_any(ev->window))
        return;

    if (!find_client_by_win(ev->window)) {
        manage(ev->window);
    }
}

static void handle_destroy_notify(XEvent *e) {
    XDestroyWindowEvent *ev = &e->xdestroywindow;
    Client *c = find_client_by_win(ev->window);
    if (c) {
        unmanage(c);
    }
}

static void handle_unmap_notify(XEvent *e) {
    XUnmapEvent *ev = &e->xunmap;
    Client *c = find_client_by_win(ev->window);
    if (!c) return;

    // Many toolkits "close" by unmapping; treat that as gone.
    unmanage(c);
}

static void handle_configure_request(XEvent *e) {
    XConfigureRequestEvent *ev = &e->xconfigurerequest;
    Client *c = find_client_by_win(ev->window);

    XWindowChanges wc;
    wc.x = ev->x;
    wc.y = ev->y;
    wc.width = ev->width;
    wc.height = ev->height;
    wc.border_width = ev->border_width;
    wc.sibling = ev->above;
    wc.stack_mode = ev->detail;

    if (c) {
        c->w = ev->width;
        c->h = ev->height;
        XConfigureWindow(dpy, c->frame,
                         CWX | CWY | CWWidth | CWHeight | CWStackMode, &wc);
        XMoveResizeWindow(dpy, c->win, 0, TITLE_HEIGHT, c->w, c->h);
        XMoveResizeWindow(dpy, c->titlebar, 0, 0, c->w, TITLE_HEIGHT);
        draw_title(c);
        draw_taskbar();
    } else {
        XConfigureWindow(dpy, ev->window, ev->value_mask, &wc);
    }
}

static void handle_client_message(XEvent *e) {
    XClientMessageEvent *ev = &e->xclient;
    if (ev->message_type == WM_PROTOCOLS &&
        (Atom)ev->data.l[0] == WM_DELETE_WINDOW) {
        Client *c = find_client_by_win(ev->window);
        if (c) {
            // Ask client to close; visuals will disappear on Unmap/Destroy.
            XEvent msg = *e;
            msg.xclient.window = c->win;
            XSendEvent(dpy, c->win, False, NoEventMask, &msg);
        }
    }
}

static void handle_button_press(XEvent *e) {
    XButtonEvent *ev = &e->xbutton;

    if (ev->window == taskbar) {
        int count = 0;
        for (Client *c = clients; c; c = c->next) count++;
        if (count == 0) return;

        int sw = DisplayWidth(dpy, screen);
        int slot = sw / count;
        int index = ev->x / (slot ? slot : 1);

        int i = 0;
        for (Client *c = clients; c; c = c->next, i++) {
            if (i == index) {
                focus_client(c);
                draw_taskbar();
                break;
            }
        }
        return;
    }

    Client *c = find_client_any(ev->window);
    if (!c) return;

    // Alt + RightClick anywhere on the window: resize from bottom-right.
    if ((ev->state & Mod1Mask) && ev->button == Button3) {
        focus_client(c);
        draw_taskbar();
        start_resize(c, ev->x_root, ev->y_root);
        return;
    }

    // click on client content: focus + let client handle the click
    if (ev->window == c->win && ev->button == Button1) {
        focus_client(c);
        draw_taskbar();
        return;
    }

    if (ev->window == c->titlebar && ev->button == Button1) {
        // close button
        if (ev->x >= c->w - CLOSE_WIDTH) {
            if (WM_DELETE_WINDOW != None) {
                XEvent msg;
                memset(&msg, 0, sizeof(msg));
                msg.xclient.type = ClientMessage;
                msg.xclient.window = c->win;
                msg.xclient.message_type = WM_PROTOCOLS;
                msg.xclient.format = 32;
                msg.xclient.data.l[0] = WM_DELETE_WINDOW;
                msg.xclient.data.l[1] = CurrentTime;
                XSendEvent(dpy, c->win, False, NoEventMask, &msg);
            } else {
                // Fallback: kill client if it doesn't support WM_DELETE_WINDOW.
                XKillClient(dpy, c->win);
            }
            // Wait for Unmap/Destroy to unmanage.
            return;
        }

        // start move
        int start_x = ev->x_root;
        int start_y = ev->y_root;
        int orig_x = c->x;
        int orig_y = c->y;

        focus_client(c);
        draw_taskbar();

        XGrabPointer(dpy, root, False,
                     PointerMotionMask | ButtonReleaseMask,
                     GrabModeAsync, GrabModeAsync,
                     None, None, CurrentTime);

        for (;;) {
            XEvent ev2;
            XNextEvent(dpy, &ev2);
            if (ev2.type == MotionNotify) {
                XMotionEvent *mv = &ev2.xmotion;
                int dx = mv->x_root - start_x;
                int dy = mv->y_root - start_y;
                c->x = orig_x + dx;
                c->y = orig_y + dy;
                XMoveWindow(dpy, c->frame, c->x, c->y);
            } else if (ev2.type == ButtonRelease) {
                break;
            } else {
                // Don't drop Expose/Map/etc. while moving; handle them.
                dispatch_event(&ev2);
            }
        }

        XUngrabPointer(dpy, CurrentTime);
        return;
    }

    if (ev->window == c->frame && ev->button == Button1) {
        // For now: just focus on frame click.
        focus_client(c);
        draw_taskbar();
    }
}

static void handle_expose(XEvent *e) {
    XExposeEvent *ev = &e->xexpose;
    if (ev->window == taskbar) {
        draw_taskbar();
        return;
    }
    Client *c = find_client_any(ev->window);
    if (c && ev->window == c->titlebar) {
        draw_title(c);
    }
}

static void handle_property_notify(XEvent *e) {
    XPropertyEvent *ev = &e->xproperty;
    if (ev->state == PropertyNewValue && ev->atom == WM_NAME_ATOM) {
        Client *c = find_client_by_win(ev->window);
        if (c) {
            update_client_name(c);
            draw_title(c);
            draw_taskbar();
        }
    }
}

/* ---------- Setup ---------- */

static void setup_colors(void) {
    Colormap cmap = DefaultColormap(dpy, screen);
    XColor col;

    XParseColor(dpy, cmap, "#3b4252", &col);
    XAllocColor(dpy, cmap, &col);
    bg_color = col.pixel;

    XParseColor(dpy, cmap, "#88c0d0", &col);
    XAllocColor(dpy, cmap, &col);
    border_focus = col.pixel;

    XParseColor(dpy, cmap, "#4c566a", &col);
    XAllocColor(dpy, cmap, &col);
    border_unfocus = col.pixel;

    XParseColor(dpy, cmap, "#2e3440", &col);
    XAllocColor(dpy, cmap, &col);
    title_bg = col.pixel;

    XParseColor(dpy, cmap, "#eceff4", &col);
    XAllocColor(dpy, cmap, &col);
    title_fg = col.pixel;

    XParseColor(dpy, cmap, "#3b4252", &col);
    XAllocColor(dpy, cmap, &col);
    taskbar_bg = col.pixel;

    XParseColor(dpy, cmap, "#d8dee9", &col);
    XAllocColor(dpy, cmap, &col);
    taskbar_fg = col.pixel;
}

static void setup_taskbar(void) {
    int sw = DisplayWidth(dpy, screen);
    int sh = DisplayHeight(dpy, screen);

    XSetWindowAttributes wa;
    wa.override_redirect = True;  // don't manage our own taskbar
    wa.background_pixel = taskbar_bg;
    wa.border_pixel = border_unfocus;

    taskbar = XCreateWindow(
        dpy, root,
        0, sh - TASKBAR_HEIGHT,
        sw, TASKBAR_HEIGHT,
        0,
        DefaultDepth(dpy, screen),
        CopyFromParent,
        DefaultVisual(dpy, screen),
        CWOverrideRedirect | CWBackPixel | CWBorderPixel,
        &wa
    );

    XSelectInput(dpy, taskbar,
                 ExposureMask |
                 ButtonPressMask |
                 ButtonReleaseMask);

    XMapWindow(dpy, taskbar);
}

/* ---------- main ---------- */

int main(void) {
    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "Cannot open display\n");
        return 1;
    }

    XSetErrorHandler(xerror);

    screen = DefaultScreen(dpy);
    root = RootWindow(dpy, screen);

    setup_colors();

    WM_DELETE_WINDOW = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    WM_PROTOCOLS     = XInternAtom(dpy, "WM_PROTOCOLS", False);
    WM_NAME_ATOM     = XInternAtom(dpy, "WM_NAME", False);

    XSelectInput(dpy, root,
                 SubstructureRedirectMask |
                 ButtonPressMask |
                 ButtonReleaseMask |
                 PointerMotionMask |
                 PropertyChangeMask);

    setup_taskbar();

    for (;;) {
        XEvent e;
        XNextEvent(dpy, &e);

        switch (e.type) {
        case MapRequest:
            handle_map_request(&e);
            break;
        case DestroyNotify:
            handle_destroy_notify(&e);
            break;
        case UnmapNotify:
            handle_unmap_notify(&e);
            break;
        case ConfigureRequest:
            handle_configure_request(&e);
            break;
        case ClientMessage:
            handle_client_message(&e);
            break;
        case ButtonPress:
            handle_button_press(&e);
            break;
        case Expose:
            handle_expose(&e);
            break;
        case PropertyNotify:
            handle_property_notify(&e);
            break;
        default:
            break;
        }
    }

    XCloseDisplay(dpy);
    return 0;
}
