// main.c - CalWM: a tiny floating Xlib window manager
// Features:
//  - Frame windows with titlebars
//  - Draggable windows (left mouse on titlebar only)
//  - Resizable windows (right mouse on frame)
//  - Close button in titlebar
//  - Simple taskbar with clickable window buttons
//  - Input focus on click
//  - Raise on focus
//  - Mod4 + Q to quit
//  - Global X error handler (BadWindow-safe, xterm/xeyes-safe)

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TITLE_HEIGHT      24
#define BORDER_WIDTH      2
#define CLOSE_SIZE        18
#define TASKBAR_HEIGHT    28
#define TASK_BUTTON_WIDTH 140

typedef struct Client {
    Window win;      // client window
    Window frame;    // frame window (with titlebar)
    int x, y, w, h;  // client geometry
    struct Client *next;
} Client;

static Display *dpy;
static int screen;
static Window root;
static Window taskbar = 0;
static unsigned int modkey = Mod1Mask; // alt key

static Client *clients = NULL;
static Client *focused = NULL;
static int running = 1;

static GC gc;
static unsigned long fg_color;
static unsigned long bg_color;
static unsigned long border_focus;
static unsigned long border_unfocus;

// ---------- Global X error handler ----------

static int xerror(Display *d, XErrorEvent *e) {
    // Swallow common async errors that happen when windows die:
    // BadWindow, BadMatch, BadDrawable, etc.
    switch (e->error_code) {
    case BadWindow:
    case BadMatch:
    case BadDrawable:
        return 0;
    default:
        fprintf(stderr,
                "XError: code=%d req=%d minor=%d resource=0x%lx\n",
                e->error_code,
                e->request_code,
                e->minor_code,
                e->resourceid);
        return 0;
    }
}

// ---------- Client lookup ----------

static Client *find_client_by_window(Window w) {
    for (Client *c = clients; c; c = c->next) {
        if (c->win == w || c->frame == w)
            return c;
    }
    return NULL;
}

// Forward declarations
static void draw_title(Client *c);
static void draw_taskbar(void);

// ---------- Taskbar ----------

static void draw_taskbar(void) {
    if (!taskbar)
        return;

    XClearWindow(dpy, taskbar);

    int x = 4;
    for (Client *c = clients; c; c = c->next) {
        int w = TASK_BUTTON_WIDTH;

        if (c == focused) {
            XSetForeground(dpy, gc, border_focus);
        } else {
            XSetForeground(dpy, gc, border_unfocus);
        }

        XFillRectangle(dpy, taskbar, gc, x, 4, w, TASKBAR_HEIGHT - 8);

        char *name = NULL;
        if (!XFetchName(dpy, c->win, &name) || !name) {
            name = "Window";
        }

        XSetForeground(dpy, gc, bg_color);
        XDrawString(dpy, taskbar, gc, x + 6, TASKBAR_HEIGHT - 10,
                    name, strlen(name));

        if (name && strcmp(name, "Window") != 0)
            XFree(name);

        x += w + 4;
    }
}

// ---------- Focus ----------

static void focus_client(Client *c) {
    if (!c) return;

    focused = c;

    XSetInputFocus(dpy, c->win, RevertToPointerRoot, CurrentTime);
    XRaiseWindow(dpy, c->frame);

    for (Client *it = clients; it; it = it->next) {
        if (it == c) {
            XSetWindowBorder(dpy, it->frame, border_focus);
        } else {
            XSetWindowBorder(dpy, it->frame, border_unfocus);
        }
        draw_title(it);
    }

    draw_taskbar();
}

// ---------- Client list management ----------

static void add_client(Client *c) {
    c->next = clients;
    clients = c;
    draw_taskbar();
}

static void remove_client(Client *c) {
    if (!c) return;
    Client **pc = &clients;
    while (*pc) {
        if (*pc == c) {
            *pc = c->next;
            free(c);
            draw_taskbar();
            return;
        }
        pc = &(*pc)->next;
    }
}

// ---------- Unmanage ----------

static void unmanage(Window w) {
    Client *c = find_client_by_window(w);
    if (!c) return;

    XUnmapWindow(dpy, c->frame);
    XReparentWindow(dpy, c->win, root, c->x, c->y);
    XRemoveFromSaveSet(dpy, c->win);
    XDestroyWindow(dpy, c->frame);

    if (focused == c)
        focused = NULL;

    remove_client(c);
}

// ---------- Title drawing ----------

static void draw_title(Client *c) {
    if (!c) return;

    XClearWindow(dpy, c->frame);

    XSetForeground(dpy, gc, bg_color);
    XFillRectangle(dpy, c->frame, gc,
                   0, 0, c->w, TITLE_HEIGHT);

    char *name = NULL;
    if (!XFetchName(dpy, c->win, &name) || !name) {
        name = "CalWM";
    }

    XSetForeground(dpy, gc, fg_color);
    int baseline = TITLE_HEIGHT / 2 + 5;
    XDrawString(dpy, c->frame, gc, 8, baseline,
                name, strlen(name));

    if (name && strcmp(name, "CalWM") != 0)
        XFree(name);

    int bx = c->w - CLOSE_SIZE - 4;
    int by = (TITLE_HEIGHT - CLOSE_SIZE) / 2;

    XSetForeground(dpy, gc, fg_color);
    XFillRectangle(dpy, c->frame, gc,
                   bx, by, CLOSE_SIZE, CLOSE_SIZE);

    XSetForeground(dpy, gc, bg_color);
    XDrawLine(dpy, c->frame, gc,
              bx + 3, by + 3,
              bx + CLOSE_SIZE - 3, by + CLOSE_SIZE - 3);
    XDrawLine(dpy, c->frame, gc,
              bx + CLOSE_SIZE - 3, by + 3,
              bx + 3, by + CLOSE_SIZE - 3);
}

// ---------- Manage new window ----------

static void manage(Window w) {
    XWindowAttributes attr;
    if (!XGetWindowAttributes(dpy, w, &attr) || attr.override_redirect)
        return;

    Client *c = calloc(1, sizeof(Client));
    if (!c) return;

    c->win = w;
    c->x = attr.x;
    c->y = attr.y;
    c->w = attr.width;
    c->h = attr.height;

    unsigned long border = border_unfocus;

    c->frame = XCreateSimpleWindow(
        dpy, root,
        c->x, c->y,
        c->w,
        c->h + TITLE_HEIGHT,
        BORDER_WIDTH,
        border,
        bg_color
    );

    XSelectInput(dpy, c->win,
                 ExposureMask |
                 ButtonPressMask |
                 ButtonReleaseMask |
                 PointerMotionMask |
                 SubstructureRedirectMask |
                 SubstructureNotifyMask);

    XAddToSaveSet(dpy, c->win);
    XReparentWindow(dpy, c->win, c->frame, 0, TITLE_HEIGHT);

    XGrabButton(dpy, Button1, AnyModifier, c->titlebar, True,
                ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                GrabModeAsync, GrabModeAsync, None, None);

    XGrabButton(dpy, Button3, AnyModifier, c->titlebar, True,
                ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                GrabModeAsync, GrabModeAsync, None, None);

    XMapWindow(dpy, c->win);
    XMapWindow(dpy, c->frame);

    add_client(c);
    focus_client(c);
}

// ---------- Setup ----------

static void setup_colors_and_gc(void) {
    screen = DefaultScreen(dpy);
    root = RootWindow(dpy, screen);

    fg_color = BlackPixel(dpy, screen);
    bg_color = WhitePixel(dpy, screen);

    border_focus = BlackPixel(dpy, screen);
    border_unfocus = BlackPixel(dpy, screen);

    gc = XCreateGC(dpy, root, 0, NULL);
    XSetForeground(dpy, gc, fg_color);
    XSetBackground(dpy, gc, bg_color);
}

static void create_taskbar(void) {
    int sw = DisplayWidth(dpy, screen);
    int sh = DisplayHeight(dpy, screen);

    taskbar = XCreateSimpleWindow(
        dpy, root,
        0, sh - TASKBAR_HEIGHT,
        sw, TASKBAR_HEIGHT,
        0,
        BlackPixel(dpy, screen),
        WhitePixel(dpy, screen)
    );

    XSelectInput(dpy, taskbar,
                 ExposureMask |
                 ButtonPressMask);

    XMapWindow(dpy, taskbar);
}

static void grab_keys(void) {
    XUngrabKey(dpy, AnyKey, AnyModifier, root);

    KeyCode q = XKeysymToKeycode(dpy, XStringToKeysym("q"));
    XGrabKey(dpy, q, modkey, root, True,
             GrabModeAsync, GrabModeAsync);
}

// ---------- main ----------

int main(void) {
    printf("Starting up... \n");
    XEvent ev;
    int moving = 0, resizing = 0;
    int start_x = 0, start_y = 0;
    int orig_x = 0, orig_y = 0, orig_w = 0, orig_h = 0;
    Client *drag_client = NULL;
    printf("Connecting to the X11 server... \n");
    dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "Failed to connect to X server\n");
        return 1;
    }

    XSetErrorHandler(xerror);

    screen = DefaultScreen(dpy);
    root = RootWindow(dpy, screen);

    printf("Connected to X server\n");

    setup_colors_and_gc();
    grab_keys();
    create_taskbar();

    XSelectInput(dpy, root,
                 SubstructureRedirectMask |
                 SubstructureNotifyMask |
                 ButtonPressMask |
                 KeyPressMask);

    XSync(dpy, False);

    while (running && !XNextEvent(dpy, &ev)) {
        switch (ev.type) {
        case MapRequest: {
            XMapRequestEvent *e = &ev.xmaprequest;
            if (!find_client_by_window(e->window))
                manage(e->window);
            break;
        }

        case ConfigureRequest: {
            XConfigureRequestEvent *e = &ev.xconfigurerequest;
            Client *c = find_client_by_window(e->window);
            XWindowChanges wc;

            if (c) {
                if (e->value_mask & CWX) c->x = e->x;
                if (e->value_mask & CWY) c->y = e->y;
                if (e->value_mask & CWWidth) c->w = e->width;
                if (e->value_mask & CWHeight) c->h = e->height;

                wc.x = c->x;
                wc.y = c->y;
                wc.width = c->w;
                wc.height = c->h + TITLE_HEIGHT;
                wc.border_width = BORDER_WIDTH;
                wc.sibling = e->above;
                wc.stack_mode = e->detail;

                XConfigureWindow(dpy, c->frame, e->value_mask, &wc);
                XMoveResizeWindow(dpy, c->win,
                                  0, TITLE_HEIGHT,
                                  c->w, c->h);
                draw_title(c);
            } else {
                wc.x = e->x;
                wc.y = e->y;
                wc.width = e->width;
                wc.height = e->height;
                wc.border_width = e->border_width;
                wc.sibling = e->above;
                wc.stack_mode = e->detail;
                XConfigureWindow(dpy, e->window, e->value_mask, &wc);
            }
            break;
        }

        case DestroyNotify: {
            XDestroyWindowEvent *e = &ev.xdestroywindow;
            unmanage(e->window);
            break;
        }

        case UnmapNotify: {
            XUnmapEvent *e = &ev.xunmap;
            Client *c = find_client_by_window(e->window);
            if (!c) break;

            if (e->window == c->win)
                unmanage(e->window);
            break;
        }

        case Expose: {
            XExposeEvent *e = &ev.xexpose;
            if (taskbar && e->window == taskbar) {
                if (e->count == 0)
                    draw_taskbar();
            } else {
                Client *c = find_client_by_window(e->window);
                if (c && e->count == 0)
                    draw_title(c);
            }
            break;
        }

        case ButtonPress: {
            XButtonEvent *e = &ev.xbutton;

            // Taskbar click
            if (taskbar && e->window == taskbar) {
                int x = e->x;
                int pos = 4;
                for (Client *c = clients; c; c = c->next) {
                    if (x >= pos && x <= pos + TASK_BUTTON_WIDTH) {
                        focus_client(c);
                        break;
                    }
                    pos += TASK_BUTTON_WIDTH + 4;
                }
                break;
            }

            Client *c = find_client_by_window(e->window);
            if (!c) break;

            focus_client(c);

            // Close button
            if (e->window == c->frame &&
                e->y >= 0 && e->y <= TITLE_HEIGHT) {

                int bx1 = c->w - CLOSE_SIZE - 4;
                int bx2 = c->w - 4;
                int by1 = (TITLE_HEIGHT - CLOSE_SIZE) / 2;
                int by2 = by1 + CLOSE_SIZE;

                int in_close =
                    (e->x >= bx1 && e->x <= bx2 &&
                     e->y >= by1 && e->y <= by2);

                if (in_close && e->button == Button1) {
                    Atom wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
                    Atom wm_protocols = XInternAtom(dpy, "WM_PROTOCOLS", True);

                    if (wm_protocols != None) {
                        XEvent msg;
                        memset(&msg, 0, sizeof(msg));
                        msg.xclient.type = ClientMessage;
                        msg.xclient.window = c->win;
                        msg.xclient.message_type = wm_protocols;
                        msg.xclient.format = 32;
                        msg.xclient.data.l[0] = wm_delete;
                        msg.xclient.data.l[1] = CurrentTime;
                        XSendEvent(dpy, c->win, False, NoEventMask, &msg);
                    } else {
                        XKillClient(dpy, c->win);
                    }
                    break;
                }

                // Start move only if in titlebar but NOT in close button
                int in_title = (e->y >= 0 && e->y < TITLE_HEIGHT);
                if (in_title && !in_close && e->button == Button1) {
                    drag_client = c;
                    moving = 1;
                    resizing = 0;

                    start_x = e->x_root;
                    start_y = e->y_root;

                    orig_x = c->x;
                    orig_y = c->y;
                    orig_w = c->w;
                    orig_h = c->h;
                    break;
                }
            }

            // Start resize on right-click anywhere on frame
            if (e->window == c->frame && e->button == Button3) {
                drag_client = c;
                moving = 0;
                resizing = 1;

                start_x = e->x_root;
                start_y = e->y_root;

                orig_x = c->x;
                orig_y = c->y;
                orig_w = c->w;
                orig_h = c->h;
            }

            break;
        }

        case MotionNotify: {
            XMotionEvent *e = &ev.xmotion;
            if (!drag_client) break;

            int dx = e->x_root - start_x;
            int dy = e->y_root - start_y;

            if (moving) {
                drag_client->x = orig_x + dx;
                drag_client->y = orig_y + dy;
                XMoveWindow(dpy, drag_client->frame,
                            drag_client->x, drag_client->y);
            } else if (resizing) {
                int nw = orig_w + dx;
                int nh = orig_h + dy;
                if (nw < 50) nw = 50;
                if (nh < 50) nh = 50;
                drag_client->w = nw;
                drag_client->h = nh;
                XMoveResizeWindow(dpy, drag_client->frame,
                                  drag_client->x, drag_client->y,
                                  drag_client->w,
                                  drag_client->h + TITLE_HEIGHT);
                XMoveResizeWindow(dpy, drag_client->win,
                                  0, TITLE_HEIGHT,
                                  drag_client->w,
                                  drag_client->h);
                draw_title(drag_client);
            }
            break;
        }

        case ButtonRelease: {
            moving = 0;
            resizing = 0;
            drag_client = NULL;
            break;
        }

        case KeyPress: {
            XKeyEvent *e = &ev.xkey;
            KeySym sym = XLookupKeysym(e, 0);
            if ((e->state & modkey) && sym == XK_q) {
                running = 0;
            }
            break;
        }
        }
    }

    XCloseDisplay(dpy);
    return 0;
}
