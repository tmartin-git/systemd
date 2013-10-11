/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2013 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <endian.h>
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/poll.h>
#include <byteswap.h>
#include <sys/mman.h>
#include <pthread.h>

#include "util.h"
#include "macro.h"
#include "strv.h"
#include "set.h"
#include "missing.h"

#include "sd-bus.h"
#include "bus-internal.h"
#include "bus-message.h"
#include "bus-type.h"
#include "bus-socket.h"
#include "bus-kernel.h"
#include "bus-control.h"
#include "bus-introspect.h"
#include "bus-signature.h"

static int bus_poll(sd_bus *bus, bool need_more, uint64_t timeout_usec);

static void bus_close_fds(sd_bus *b) {
        assert(b);

        if (b->input_fd >= 0)
                close_nointr_nofail(b->input_fd);

        if (b->output_fd >= 0 && b->output_fd != b->input_fd)
                close_nointr_nofail(b->output_fd);

        b->input_fd = b->output_fd = -1;
}

static void bus_node_destroy(sd_bus *b, struct node *n) {
        struct node_callback *c;
        struct node_vtable *v;
        struct node_enumerator *e;

        assert(b);

        if (!n)
                return;

        while (n->child)
                bus_node_destroy(b, n->child);

        while ((c = n->callbacks)) {
                LIST_REMOVE(struct node_callback, callbacks, n->callbacks, c);
                free(c);
        }

        while ((v = n->vtables)) {
                LIST_REMOVE(struct node_vtable, vtables, n->vtables, v);
                free(v->interface);
                free(v);
        }

        while ((e = n->enumerators)) {
                LIST_REMOVE(struct node_enumerator, enumerators, n->enumerators, e);
                free(e);
        }

        if (n->parent)
                LIST_REMOVE(struct node, siblings, n->parent->child, n);

        assert_se(hashmap_remove(b->nodes, n->path) == n);
        free(n->path);
        free(n);
}

static void bus_free(sd_bus *b) {
        struct filter_callback *f;
        struct node *n;
        unsigned i;

        assert(b);

        bus_close_fds(b);

        if (b->kdbus_buffer)
                munmap(b->kdbus_buffer, KDBUS_POOL_SIZE);

        free(b->rbuffer);
        free(b->unique_name);
        free(b->auth_buffer);
        free(b->address);
        free(b->kernel);

        free(b->exec_path);
        strv_free(b->exec_argv);

        close_many(b->fds, b->n_fds);
        free(b->fds);

        for (i = 0; i < b->rqueue_size; i++)
                sd_bus_message_unref(b->rqueue[i]);
        free(b->rqueue);

        for (i = 0; i < b->wqueue_size; i++)
                sd_bus_message_unref(b->wqueue[i]);
        free(b->wqueue);

        hashmap_free_free(b->reply_callbacks);
        prioq_free(b->reply_callbacks_prioq);

        while ((f = b->filter_callbacks)) {
                LIST_REMOVE(struct filter_callback, callbacks, b->filter_callbacks, f);
                free(f);
        }

        bus_match_free(&b->match_callbacks);

        hashmap_free_free(b->vtable_methods);
        hashmap_free_free(b->vtable_properties);

        while ((n = hashmap_first(b->nodes)))
                bus_node_destroy(b, n);

        hashmap_free(b->nodes);

        bus_kernel_flush_memfd(b);

        assert_se(pthread_mutex_destroy(&b->memfd_cache_mutex) == 0);

        free(b);
}

int sd_bus_new(sd_bus **ret) {
        sd_bus *r;

        if (!ret)
                return -EINVAL;

        r = new0(sd_bus, 1);
        if (!r)
                return -ENOMEM;

        r->n_ref = REFCNT_INIT;
        r->input_fd = r->output_fd = -1;
        r->message_version = 1;
        r->hello_flags |= KDBUS_HELLO_ACCEPT_FD;
        r->original_pid = getpid();

        assert_se(pthread_mutex_init(&r->memfd_cache_mutex, NULL) == 0);

        /* We guarantee that wqueue always has space for at least one
         * entry */
        r->wqueue = new(sd_bus_message*, 1);
        if (!r->wqueue) {
                free(r);
                return -ENOMEM;
        }

        *ret = r;
        return 0;
}

int sd_bus_set_address(sd_bus *bus, const char *address) {
        char *a;

        if (!bus)
                return -EINVAL;
        if (bus->state != BUS_UNSET)
                return -EPERM;
        if (!address)
                return -EINVAL;
        if (bus_pid_changed(bus))
                return -ECHILD;

        a = strdup(address);
        if (!a)
                return -ENOMEM;

        free(bus->address);
        bus->address = a;

        return 0;
}

int sd_bus_set_fd(sd_bus *bus, int input_fd, int output_fd) {
        if (!bus)
                return -EINVAL;
        if (bus->state != BUS_UNSET)
                return -EPERM;
        if (input_fd < 0)
                return -EINVAL;
        if (output_fd < 0)
                return -EINVAL;
        if (bus_pid_changed(bus))
                return -ECHILD;

        bus->input_fd = input_fd;
        bus->output_fd = output_fd;
        return 0;
}

int sd_bus_set_exec(sd_bus *bus, const char *path, char *const argv[]) {
        char *p, **a;

        if (!bus)
                return -EINVAL;
        if (bus->state != BUS_UNSET)
                return -EPERM;
        if (!path)
                return -EINVAL;
        if (strv_isempty(argv))
                return -EINVAL;
        if (bus_pid_changed(bus))
                return -ECHILD;

        p = strdup(path);
        if (!p)
                return -ENOMEM;

        a = strv_copy(argv);
        if (!a) {
                free(p);
                return -ENOMEM;
        }

        free(bus->exec_path);
        strv_free(bus->exec_argv);

        bus->exec_path = p;
        bus->exec_argv = a;

        return 0;
}

int sd_bus_set_bus_client(sd_bus *bus, int b) {
        if (!bus)
                return -EINVAL;
        if (bus->state != BUS_UNSET)
                return -EPERM;
        if (bus_pid_changed(bus))
                return -ECHILD;

        bus->bus_client = !!b;
        return 0;
}

int sd_bus_negotiate_fds(sd_bus *bus, int b) {
        if (!bus)
                return -EINVAL;
        if (bus->state != BUS_UNSET)
                return -EPERM;
        if (bus_pid_changed(bus))
                return -ECHILD;

        SET_FLAG(bus->hello_flags, KDBUS_HELLO_ACCEPT_FD, b);
        return 0;
}

int sd_bus_negotiate_attach_comm(sd_bus *bus, int b) {
        if (!bus)
                return -EINVAL;
        if (bus->state != BUS_UNSET)
                return -EPERM;
        if (bus_pid_changed(bus))
                return -ECHILD;

        SET_FLAG(bus->hello_flags, KDBUS_HELLO_ATTACH_COMM, b);
        return 0;
}

int sd_bus_negotiate_attach_exe(sd_bus *bus, int b) {
        if (!bus)
                return -EINVAL;
        if (bus->state != BUS_UNSET)
                return -EPERM;
        if (bus_pid_changed(bus))
                return -ECHILD;

        SET_FLAG(bus->hello_flags, KDBUS_HELLO_ATTACH_EXE, b);
        return 0;
}

int sd_bus_negotiate_attach_cmdline(sd_bus *bus, int b) {
        if (!bus)
                return -EINVAL;
        if (bus->state != BUS_UNSET)
                return -EPERM;
        if (bus_pid_changed(bus))
                return -ECHILD;

        SET_FLAG(bus->hello_flags, KDBUS_HELLO_ATTACH_CMDLINE, b);
        return 0;
}

int sd_bus_negotiate_attach_cgroup(sd_bus *bus, int b) {
        if (!bus)
                return -EINVAL;
        if (bus->state != BUS_UNSET)
                return -EPERM;
        if (bus_pid_changed(bus))
                return -ECHILD;

        SET_FLAG(bus->hello_flags, KDBUS_HELLO_ATTACH_CGROUP, b);
        return 0;
}

int sd_bus_negotiate_attach_caps(sd_bus *bus, int b) {
        if (!bus)
                return -EINVAL;
        if (bus->state != BUS_UNSET)
                return -EPERM;
        if (bus_pid_changed(bus))
                return -ECHILD;

        SET_FLAG(bus->hello_flags, KDBUS_HELLO_ATTACH_CAPS, b);
        return 0;
}

int sd_bus_negotiate_attach_selinux_context(sd_bus *bus, int b) {
        if (!bus)
                return -EINVAL;
        if (bus->state != BUS_UNSET)
                return -EPERM;
        if (bus_pid_changed(bus))
                return -ECHILD;

        SET_FLAG(bus->hello_flags, KDBUS_HELLO_ATTACH_SECLABEL, b);
        return 0;
}

int sd_bus_negotiate_attach_audit(sd_bus *bus, int b) {
        if (!bus)
                return -EINVAL;
        if (bus->state != BUS_UNSET)
                return -EPERM;
        if (bus_pid_changed(bus))
                return -ECHILD;

        SET_FLAG(bus->hello_flags, KDBUS_HELLO_ATTACH_AUDIT, b);
        return 0;
}

int sd_bus_set_server(sd_bus *bus, int b, sd_id128_t server_id) {
        if (!bus)
                return -EINVAL;
        if (!b && !sd_id128_equal(server_id, SD_ID128_NULL))
                return -EINVAL;
        if (bus->state != BUS_UNSET)
                return -EPERM;
        if (bus_pid_changed(bus))
                return -ECHILD;

        bus->is_server = !!b;
        bus->server_id = server_id;
        return 0;
}

int sd_bus_set_anonymous(sd_bus *bus, int b) {
        if (!bus)
                return -EINVAL;
        if (bus->state != BUS_UNSET)
                return -EPERM;
        if (bus_pid_changed(bus))
                return -ECHILD;

        bus->anonymous_auth = !!b;
        return 0;
}

static int hello_callback(sd_bus *bus, sd_bus_message *reply, void *userdata) {
        const char *s;
        int r;

        assert(bus);
        assert(bus->state == BUS_HELLO);
        assert(reply);

        r = bus_message_to_errno(reply);
        if (r < 0)
                return r;

        r = sd_bus_message_read(reply, "s", &s);
        if (r < 0)
                return r;

        if (!service_name_is_valid(s) || s[0] != ':')
                return -EBADMSG;

        bus->unique_name = strdup(s);
        if (!bus->unique_name)
                return -ENOMEM;

        bus->state = BUS_RUNNING;

        return 1;
}

static int bus_send_hello(sd_bus *bus) {
        _cleanup_bus_message_unref_ sd_bus_message *m = NULL;
        int r;

        assert(bus);

        if (!bus->bus_client || bus->is_kernel)
                return 0;

        r = sd_bus_message_new_method_call(
                        bus,
                        "org.freedesktop.DBus",
                        "/",
                        "org.freedesktop.DBus",
                        "Hello",
                        &m);
        if (r < 0)
                return r;

        return sd_bus_send_with_reply(bus, m, hello_callback, NULL, 0, &bus->hello_serial);
}

int bus_start_running(sd_bus *bus) {
        assert(bus);

        if (bus->bus_client && !bus->is_kernel) {
                bus->state = BUS_HELLO;
                return 1;
        }

        bus->state = BUS_RUNNING;
        return 1;
}

static int parse_address_key(const char **p, const char *key, char **value) {
        size_t l, n = 0;
        const char *a;
        char *r = NULL;

        assert(p);
        assert(*p);
        assert(value);

        if (key) {
                l = strlen(key);
                if (strncmp(*p, key, l) != 0)
                        return 0;

                if ((*p)[l] != '=')
                        return 0;

                if (*value)
                        return -EINVAL;

                a = *p + l + 1;
        } else
                a = *p;

        while (*a != ';' && *a != ',' && *a != 0) {
                char c, *t;

                if (*a == '%') {
                        int x, y;

                        x = unhexchar(a[1]);
                        if (x < 0) {
                                free(r);
                                return x;
                        }

                        y = unhexchar(a[2]);
                        if (y < 0) {
                                free(r);
                                return y;
                        }

                        c = (char) ((x << 4) | y);
                        a += 3;
                } else {
                        c = *a;
                        a++;
                }

                t = realloc(r, n + 2);
                if (!t) {
                        free(r);
                        return -ENOMEM;
                }

                r = t;
                r[n++] = c;
        }

        if (!r) {
                r = strdup("");
                if (!r)
                        return -ENOMEM;
        } else
                r[n] = 0;

        if (*a == ',')
                a++;

        *p = a;

        free(*value);
        *value = r;

        return 1;
}

static void skip_address_key(const char **p) {
        assert(p);
        assert(*p);

        *p += strcspn(*p, ",");

        if (**p == ',')
                (*p) ++;
}

static int parse_unix_address(sd_bus *b, const char **p, char **guid) {
        _cleanup_free_ char *path = NULL, *abstract = NULL;
        size_t l;
        int r;

        assert(b);
        assert(p);
        assert(*p);
        assert(guid);

        while (**p != 0 && **p != ';') {
                r = parse_address_key(p, "guid", guid);
                if (r < 0)
                        return r;
                else if (r > 0)
                        continue;

                r = parse_address_key(p, "path", &path);
                if (r < 0)
                        return r;
                else if (r > 0)
                        continue;

                r = parse_address_key(p, "abstract", &abstract);
                if (r < 0)
                        return r;
                else if (r > 0)
                        continue;

                skip_address_key(p);
        }

        if (!path && !abstract)
                return -EINVAL;

        if (path && abstract)
                return -EINVAL;

        if (path) {
                l = strlen(path);
                if (l > sizeof(b->sockaddr.un.sun_path))
                        return -E2BIG;

                b->sockaddr.un.sun_family = AF_UNIX;
                strncpy(b->sockaddr.un.sun_path, path, sizeof(b->sockaddr.un.sun_path));
                b->sockaddr_size = offsetof(struct sockaddr_un, sun_path) + l;
        } else if (abstract) {
                l = strlen(abstract);
                if (l > sizeof(b->sockaddr.un.sun_path) - 1)
                        return -E2BIG;

                b->sockaddr.un.sun_family = AF_UNIX;
                b->sockaddr.un.sun_path[0] = 0;
                strncpy(b->sockaddr.un.sun_path+1, abstract, sizeof(b->sockaddr.un.sun_path)-1);
                b->sockaddr_size = offsetof(struct sockaddr_un, sun_path) + 1 + l;
        }

        return 0;
}

static int parse_tcp_address(sd_bus *b, const char **p, char **guid) {
        _cleanup_free_ char *host = NULL, *port = NULL, *family = NULL;
        int r;
        struct addrinfo *result, hints = {
                .ai_socktype = SOCK_STREAM,
                .ai_flags = AI_ADDRCONFIG,
        };

        assert(b);
        assert(p);
        assert(*p);
        assert(guid);

        while (**p != 0 && **p != ';') {
                r = parse_address_key(p, "guid", guid);
                if (r < 0)
                        return r;
                else if (r > 0)
                        continue;

                r = parse_address_key(p, "host", &host);
                if (r < 0)
                        return r;
                else if (r > 0)
                        continue;

                r = parse_address_key(p, "port", &port);
                if (r < 0)
                        return r;
                else if (r > 0)
                        continue;

                r = parse_address_key(p, "family", &family);
                if (r < 0)
                        return r;
                else if (r > 0)
                        continue;

                skip_address_key(p);
        }

        if (!host || !port)
                return -EINVAL;

        if (family) {
                if (streq(family, "ipv4"))
                        hints.ai_family = AF_INET;
                else if (streq(family, "ipv6"))
                        hints.ai_family = AF_INET6;
                else
                        return -EINVAL;
        }

        r = getaddrinfo(host, port, &hints, &result);
        if (r == EAI_SYSTEM)
                return -errno;
        else if (r != 0)
                return -EADDRNOTAVAIL;

        memcpy(&b->sockaddr, result->ai_addr, result->ai_addrlen);
        b->sockaddr_size = result->ai_addrlen;

        freeaddrinfo(result);

        return 0;
}

static int parse_exec_address(sd_bus *b, const char **p, char **guid) {
        char *path = NULL;
        unsigned n_argv = 0, j;
        char **argv = NULL;
        int r;

        assert(b);
        assert(p);
        assert(*p);
        assert(guid);

        while (**p != 0 && **p != ';') {
                r = parse_address_key(p, "guid", guid);
                if (r < 0)
                        goto fail;
                else if (r > 0)
                        continue;

                r = parse_address_key(p, "path", &path);
                if (r < 0)
                        goto fail;
                else if (r > 0)
                        continue;

                if (startswith(*p, "argv")) {
                        unsigned ul;

                        errno = 0;
                        ul = strtoul(*p + 4, (char**) p, 10);
                        if (errno > 0 || **p != '=' || ul > 256) {
                                r = -EINVAL;
                                goto fail;
                        }

                        (*p) ++;

                        if (ul >= n_argv) {
                                char **x;

                                x = realloc(argv, sizeof(char*) * (ul + 2));
                                if (!x) {
                                        r = -ENOMEM;
                                        goto fail;
                                }

                                memset(x + n_argv, 0, sizeof(char*) * (ul - n_argv + 2));

                                argv = x;
                                n_argv = ul + 1;
                        }

                        r = parse_address_key(p, NULL, argv + ul);
                        if (r < 0)
                                goto fail;

                        continue;
                }

                skip_address_key(p);
        }

        if (!path) {
                r = -EINVAL;
                goto fail;
        }

        /* Make sure there are no holes in the array, with the
         * exception of argv[0] */
        for (j = 1; j < n_argv; j++)
                if (!argv[j]) {
                        r = -EINVAL;
                        goto fail;
                }

        if (argv && argv[0] == NULL) {
                argv[0] = strdup(path);
                if (!argv[0]) {
                        r = -ENOMEM;
                        goto fail;
                }
        }

        b->exec_path = path;
        b->exec_argv = argv;
        return 0;

fail:
        for (j = 0; j < n_argv; j++)
                free(argv[j]);

        free(argv);
        free(path);
        return r;
}

static int parse_kernel_address(sd_bus *b, const char **p, char **guid) {
        _cleanup_free_ char *path = NULL;
        int r;

        assert(b);
        assert(p);
        assert(*p);
        assert(guid);

        while (**p != 0 && **p != ';') {
                r = parse_address_key(p, "guid", guid);
                if (r < 0)
                        return r;
                else if (r > 0)
                        continue;

                r = parse_address_key(p, "path", &path);
                if (r < 0)
                        return r;
                else if (r > 0)
                        continue;

                skip_address_key(p);
        }

        if (!path)
                return -EINVAL;

        free(b->kernel);
        b->kernel = path;
        path = NULL;

        return 0;
}

static void bus_reset_parsed_address(sd_bus *b) {
        assert(b);

        zero(b->sockaddr);
        b->sockaddr_size = 0;
        strv_free(b->exec_argv);
        free(b->exec_path);
        b->exec_path = NULL;
        b->exec_argv = NULL;
        b->server_id = SD_ID128_NULL;
        free(b->kernel);
        b->kernel = NULL;
}

static int bus_parse_next_address(sd_bus *b) {
        _cleanup_free_ char *guid = NULL;
        const char *a;
        int r;

        assert(b);

        if (!b->address)
                return 0;
        if (b->address[b->address_index] == 0)
                return 0;

        bus_reset_parsed_address(b);

        a = b->address + b->address_index;

        while (*a != 0) {

                if (*a == ';') {
                        a++;
                        continue;
                }

                if (startswith(a, "unix:")) {
                        a += 5;

                        r = parse_unix_address(b, &a, &guid);
                        if (r < 0)
                                return r;
                        break;

                } else if (startswith(a, "tcp:")) {

                        a += 4;
                        r = parse_tcp_address(b, &a, &guid);
                        if (r < 0)
                                return r;

                        break;

                } else if (startswith(a, "unixexec:")) {

                        a += 9;
                        r = parse_exec_address(b, &a, &guid);
                        if (r < 0)
                                return r;

                        break;

                } else if (startswith(a, "kernel:")) {

                        a += 7;
                        r = parse_kernel_address(b, &a, &guid);
                        if (r < 0)
                                return r;

                        break;
                }

                a = strchr(a, ';');
                if (!a)
                        return 0;
        }

        if (guid) {
                r = sd_id128_from_string(guid, &b->server_id);
                if (r < 0)
                        return r;
        }

        b->address_index = a - b->address;
        return 1;
}

static int bus_start_address(sd_bus *b) {
        int r;

        assert(b);

        for (;;) {
                sd_bus_close(b);

                if (b->sockaddr.sa.sa_family != AF_UNSPEC) {

                        r = bus_socket_connect(b);
                        if (r >= 0)
                                return r;

                        b->last_connect_error = -r;

                } else if (b->exec_path) {

                        r = bus_socket_exec(b);
                        if (r >= 0)
                                return r;

                        b->last_connect_error = -r;
                } else if (b->kernel) {

                        r = bus_kernel_connect(b);
                        if (r >= 0)
                                return r;

                        b->last_connect_error = -r;
                }

                r = bus_parse_next_address(b);
                if (r < 0)
                        return r;
                if (r == 0)
                        return b->last_connect_error ? -b->last_connect_error : -ECONNREFUSED;
        }
}

int bus_next_address(sd_bus *b) {
        assert(b);

        bus_reset_parsed_address(b);
        return bus_start_address(b);
}

static int bus_start_fd(sd_bus *b) {
        struct stat st;
        int r;

        assert(b);
        assert(b->input_fd >= 0);
        assert(b->output_fd >= 0);

        r = fd_nonblock(b->input_fd, true);
        if (r < 0)
                return r;

        r = fd_cloexec(b->input_fd, true);
        if (r < 0)
                return r;

        if (b->input_fd != b->output_fd) {
                r = fd_nonblock(b->output_fd, true);
                if (r < 0)
                        return r;

                r = fd_cloexec(b->output_fd, true);
                if (r < 0)
                        return r;
        }

        if (fstat(b->input_fd, &st) < 0)
                return -errno;

        if (S_ISCHR(b->input_fd))
                return bus_kernel_take_fd(b);
        else
                return bus_socket_take_fd(b);
}

int sd_bus_start(sd_bus *bus) {
        int r;

        if (!bus)
                return -EINVAL;
        if (bus->state != BUS_UNSET)
                return -EPERM;
        if (bus_pid_changed(bus))
                return -ECHILD;

        bus->state = BUS_OPENING;

        if (bus->is_server && bus->bus_client)
                return -EINVAL;

        if (bus->input_fd >= 0)
                r = bus_start_fd(bus);
        else if (bus->address || bus->sockaddr.sa.sa_family != AF_UNSPEC || bus->exec_path || bus->kernel)
                r = bus_start_address(bus);
        else
                return -EINVAL;

        if (r < 0)
                return r;

        return bus_send_hello(bus);
}

int sd_bus_open_system(sd_bus **ret) {
        const char *e;
        sd_bus *b;
        int r;

        if (!ret)
                return -EINVAL;

        r = sd_bus_new(&b);
        if (r < 0)
                return r;

        e = secure_getenv("DBUS_SYSTEM_BUS_ADDRESS");
        if (e) {
                r = sd_bus_set_address(b, e);
                if (r < 0)
                        goto fail;
        } else {
                b->sockaddr.un.sun_family = AF_UNIX;
                strncpy(b->sockaddr.un.sun_path, "/run/dbus/system_bus_socket", sizeof(b->sockaddr.un.sun_path));
                b->sockaddr_size = offsetof(struct sockaddr_un, sun_path) + sizeof("/run/dbus/system_bus_socket") - 1;
        }

        b->bus_client = true;

        r = sd_bus_start(b);
        if (r < 0)
                goto fail;

        *ret = b;
        return 0;

fail:
        bus_free(b);
        return r;
}

int sd_bus_open_user(sd_bus **ret) {
        const char *e;
        sd_bus *b;
        size_t l;
        int r;

        if (!ret)
                return -EINVAL;

        r = sd_bus_new(&b);
        if (r < 0)
                return r;

        e = secure_getenv("DBUS_SESSION_BUS_ADDRESS");
        if (e) {
                r = sd_bus_set_address(b, e);
                if (r < 0)
                        goto fail;
        } else {
                e = secure_getenv("XDG_RUNTIME_DIR");
                if (!e) {
                        r = -ENOENT;
                        goto fail;
                }

                l = strlen(e);
                if (l + 4 > sizeof(b->sockaddr.un.sun_path)) {
                        r = -E2BIG;
                        goto fail;
                }

                b->sockaddr.un.sun_family = AF_UNIX;
                memcpy(mempcpy(b->sockaddr.un.sun_path, e, l), "/bus", 4);
                b->sockaddr_size = offsetof(struct sockaddr_un, sun_path) + l + 4;
        }

        b->bus_client = true;

        r = sd_bus_start(b);
        if (r < 0)
                goto fail;

        *ret = b;
        return 0;

fail:
        bus_free(b);
        return r;
}

void sd_bus_close(sd_bus *bus) {
        if (!bus)
                return;
        if (bus->state == BUS_CLOSED)
                return;
        if (bus_pid_changed(bus))
                return;

        bus->state = BUS_CLOSED;

        if (!bus->is_kernel)
                bus_close_fds(bus);

        /* We'll leave the fd open in case this is a kernel bus, since
         * there might still be memblocks around that reference this
         * bus, and they might need to invoke the
         * KDBUS_CMD_MSG_RELEASE ioctl on the fd when they are
         * freed. */
}

sd_bus *sd_bus_ref(sd_bus *bus) {
        if (!bus)
                return NULL;

        assert_se(REFCNT_INC(bus->n_ref) >= 2);

        return bus;
}

sd_bus *sd_bus_unref(sd_bus *bus) {
        if (!bus)
                return NULL;

        if (REFCNT_DEC(bus->n_ref) <= 0)
                bus_free(bus);

        return NULL;
}

int sd_bus_is_open(sd_bus *bus) {
        if (!bus)
                return -EINVAL;
        if (bus_pid_changed(bus))
                return -ECHILD;

        return BUS_IS_OPEN(bus->state);
}

int sd_bus_can_send(sd_bus *bus, char type) {
        int r;

        if (!bus)
                return -EINVAL;
        if (bus->state == BUS_UNSET)
                return -ENOTCONN;
        if (bus_pid_changed(bus))
                return -ECHILD;

        if (type == SD_BUS_TYPE_UNIX_FD) {
                if (!(bus->hello_flags & KDBUS_HELLO_ACCEPT_FD))
                        return 0;

                r = bus_ensure_running(bus);
                if (r < 0)
                        return r;

                return bus->can_fds;
        }

        return bus_type_is_valid(type);
}

int sd_bus_get_server_id(sd_bus *bus, sd_id128_t *server_id) {
        int r;

        if (!bus)
                return -EINVAL;
        if (!server_id)
                return -EINVAL;
        if (bus_pid_changed(bus))
                return -ECHILD;

        r = bus_ensure_running(bus);
        if (r < 0)
                return r;

        *server_id = bus->server_id;
        return 0;
}

static int bus_seal_message(sd_bus *b, sd_bus_message *m) {
        assert(m);

        if (m->header->version > b->message_version)
                return -EPERM;

        if (m->sealed)
                return 0;

        return bus_message_seal(m, ++b->serial);
}

static int dispatch_wqueue(sd_bus *bus) {
        int r, ret = 0;

        assert(bus);
        assert(bus->state == BUS_RUNNING || bus->state == BUS_HELLO);

        while (bus->wqueue_size > 0) {

                if (bus->is_kernel)
                        r = bus_kernel_write_message(bus, bus->wqueue[0]);
                else
                        r = bus_socket_write_message(bus, bus->wqueue[0], &bus->windex);

                if (r < 0) {
                        sd_bus_close(bus);
                        return r;
                } else if (r == 0)
                        /* Didn't do anything this time */
                        return ret;
                else if (bus->is_kernel || bus->windex >= BUS_MESSAGE_SIZE(bus->wqueue[0])) {
                        /* Fully written. Let's drop the entry from
                         * the queue.
                         *
                         * This isn't particularly optimized, but
                         * well, this is supposed to be our worst-case
                         * buffer only, and the socket buffer is
                         * supposed to be our primary buffer, and if
                         * it got full, then all bets are off
                         * anyway. */

                        sd_bus_message_unref(bus->wqueue[0]);
                        bus->wqueue_size --;
                        memmove(bus->wqueue, bus->wqueue + 1, sizeof(sd_bus_message*) * bus->wqueue_size);
                        bus->windex = 0;

                        ret = 1;
                }
        }

        return ret;
}

static int dispatch_rqueue(sd_bus *bus, sd_bus_message **m) {
        sd_bus_message *z = NULL;
        int r, ret = 0;

        assert(bus);
        assert(m);
        assert(bus->state == BUS_RUNNING || bus->state == BUS_HELLO);

        if (bus->rqueue_size > 0) {
                /* Dispatch a queued message */

                *m = bus->rqueue[0];
                bus->rqueue_size --;
                memmove(bus->rqueue, bus->rqueue + 1, sizeof(sd_bus_message*) * bus->rqueue_size);
                return 1;
        }

        /* Try to read a new message */
        do {
                if (bus->is_kernel)
                        r = bus_kernel_read_message(bus, &z);
                else
                        r = bus_socket_read_message(bus, &z);

                if (r < 0) {
                        sd_bus_close(bus);
                        return r;
                }
                if (r == 0)
                        return ret;

                ret = 1;
        } while (!z);

        *m = z;
        return ret;
}

int sd_bus_send(sd_bus *bus, sd_bus_message *m, uint64_t *serial) {
        int r;

        if (!bus)
                return -EINVAL;
        if (!BUS_IS_OPEN(bus->state))
                return -ENOTCONN;
        if (!m)
                return -EINVAL;
        if (bus_pid_changed(bus))
                return -ECHILD;

        if (m->n_fds > 0) {
                r = sd_bus_can_send(bus, SD_BUS_TYPE_UNIX_FD);
                if (r < 0)
                        return r;
                if (r == 0)
                        return -ENOTSUP;
        }

        /* If the serial number isn't kept, then we know that no reply
         * is expected */
        if (!serial && !m->sealed)
                m->header->flags |= SD_BUS_MESSAGE_NO_REPLY_EXPECTED;

        r = bus_seal_message(bus, m);
        if (r < 0)
                return r;

        /* If this is a reply and no reply was requested, then let's
         * suppress this, if we can */
        if (m->dont_send && !serial)
                return 0;

        if ((bus->state == BUS_RUNNING || bus->state == BUS_HELLO) && bus->wqueue_size <= 0) {
                size_t idx = 0;

                if (bus->is_kernel)
                        r = bus_kernel_write_message(bus, m);
                else
                        r = bus_socket_write_message(bus, m, &idx);

                if (r < 0) {
                        sd_bus_close(bus);
                        return r;
                } else if (!bus->is_kernel && idx < BUS_MESSAGE_SIZE(m))  {
                        /* Wasn't fully written. So let's remember how
                         * much was written. Note that the first entry
                         * of the wqueue array is always allocated so
                         * that we always can remember how much was
                         * written. */
                        bus->wqueue[0] = sd_bus_message_ref(m);
                        bus->wqueue_size = 1;
                        bus->windex = idx;
                }
        } else {
                sd_bus_message **q;

                /* Just append it to the queue. */

                if (bus->wqueue_size >= BUS_WQUEUE_MAX)
                        return -ENOBUFS;

                q = realloc(bus->wqueue, sizeof(sd_bus_message*) * (bus->wqueue_size + 1));
                if (!q)
                        return -ENOMEM;

                bus->wqueue = q;
                q[bus->wqueue_size ++] = sd_bus_message_ref(m);
        }

        if (serial)
                *serial = BUS_MESSAGE_SERIAL(m);

        return 0;
}

static usec_t calc_elapse(uint64_t usec) {
        if (usec == (uint64_t) -1)
                return 0;

        if (usec == 0)
                usec = BUS_DEFAULT_TIMEOUT;

        return now(CLOCK_MONOTONIC) + usec;
}

static int timeout_compare(const void *a, const void *b) {
        const struct reply_callback *x = a, *y = b;

        if (x->timeout != 0 && y->timeout == 0)
                return -1;

        if (x->timeout == 0 && y->timeout != 0)
                return 1;

        if (x->timeout < y->timeout)
                return -1;

        if (x->timeout > y->timeout)
                return 1;

        return 0;
}

int sd_bus_send_with_reply(
                sd_bus *bus,
                sd_bus_message *m,
                sd_bus_message_handler_t callback,
                void *userdata,
                uint64_t usec,
                uint64_t *serial) {

        struct reply_callback *c;
        int r;

        if (!bus)
                return -EINVAL;
        if (!BUS_IS_OPEN(bus->state))
                return -ENOTCONN;
        if (!m)
                return -EINVAL;
        if (!callback)
                return -EINVAL;
        if (m->header->type != SD_BUS_MESSAGE_TYPE_METHOD_CALL)
                return -EINVAL;
        if (m->header->flags & SD_BUS_MESSAGE_NO_REPLY_EXPECTED)
                return -EINVAL;
        if (bus_pid_changed(bus))
                return -ECHILD;

        r = hashmap_ensure_allocated(&bus->reply_callbacks, uint64_hash_func, uint64_compare_func);
        if (r < 0)
                return r;

        if (usec != (uint64_t) -1) {
                r = prioq_ensure_allocated(&bus->reply_callbacks_prioq, timeout_compare);
                if (r < 0)
                        return r;
        }

        r = bus_seal_message(bus, m);
        if (r < 0)
                return r;

        c = new0(struct reply_callback, 1);
        if (!c)
                return -ENOMEM;

        c->callback = callback;
        c->userdata = userdata;
        c->serial = BUS_MESSAGE_SERIAL(m);
        c->timeout = calc_elapse(usec);

        r = hashmap_put(bus->reply_callbacks, &c->serial, c);
        if (r < 0) {
                free(c);
                return r;
        }

        if (c->timeout != 0) {
                r = prioq_put(bus->reply_callbacks_prioq, c, &c->prioq_idx);
                if (r < 0) {
                        c->timeout = 0;
                        sd_bus_send_with_reply_cancel(bus, c->serial);
                        return r;
                }
        }

        r = sd_bus_send(bus, m, serial);
        if (r < 0) {
                sd_bus_send_with_reply_cancel(bus, c->serial);
                return r;
        }

        return r;
}

int sd_bus_send_with_reply_cancel(sd_bus *bus, uint64_t serial) {
        struct reply_callback *c;

        if (!bus)
                return -EINVAL;
        if (serial == 0)
                return -EINVAL;
        if (bus_pid_changed(bus))
                return -ECHILD;

        c = hashmap_remove(bus->reply_callbacks, &serial);
        if (!c)
                return 0;

        if (c->timeout != 0)
                prioq_remove(bus->reply_callbacks_prioq, c, &c->prioq_idx);

        free(c);
        return 1;
}

int bus_ensure_running(sd_bus *bus) {
        int r;

        assert(bus);

        if (bus->state == BUS_UNSET || bus->state == BUS_CLOSED)
                return -ENOTCONN;
        if (bus->state == BUS_RUNNING)
                return 1;

        for (;;) {
                r = sd_bus_process(bus, NULL);
                if (r < 0)
                        return r;
                if (bus->state == BUS_RUNNING)
                        return 1;
                if (r > 0)
                        continue;

                r = sd_bus_wait(bus, (uint64_t) -1);
                if (r < 0)
                        return r;
        }
}

int sd_bus_send_with_reply_and_block(
                sd_bus *bus,
                sd_bus_message *m,
                uint64_t usec,
                sd_bus_error *error,
                sd_bus_message **reply) {

        int r;
        usec_t timeout;
        uint64_t serial;
        bool room = false;

        if (!bus)
                return -EINVAL;
        if (!BUS_IS_OPEN(bus->state))
                return -ENOTCONN;
        if (!m)
                return -EINVAL;
        if (m->header->type != SD_BUS_MESSAGE_TYPE_METHOD_CALL)
                return -EINVAL;
        if (m->header->flags & SD_BUS_MESSAGE_NO_REPLY_EXPECTED)
                return -EINVAL;
        if (bus_error_is_dirty(error))
                return -EINVAL;
        if (bus_pid_changed(bus))
                return -ECHILD;

        r = bus_ensure_running(bus);
        if (r < 0)
                return r;

        r = sd_bus_send(bus, m, &serial);
        if (r < 0)
                return r;

        timeout = calc_elapse(usec);

        for (;;) {
                usec_t left;
                sd_bus_message *incoming = NULL;

                if (!room) {
                        sd_bus_message **q;

                        if (bus->rqueue_size >= BUS_RQUEUE_MAX)
                                return -ENOBUFS;

                        /* Make sure there's room for queuing this
                         * locally, before we read the message */

                        q = realloc(bus->rqueue, (bus->rqueue_size + 1) * sizeof(sd_bus_message*));
                        if (!q)
                                return -ENOMEM;

                        bus->rqueue = q;
                        room = true;
                }

                if (bus->is_kernel)
                        r = bus_kernel_read_message(bus, &incoming);
                else
                        r = bus_socket_read_message(bus, &incoming);
                if (r < 0)
                        return r;
                if (incoming) {

                        if (incoming->reply_serial == serial) {
                                /* Found a match! */

                                if (incoming->header->type == SD_BUS_MESSAGE_TYPE_METHOD_RETURN) {

                                        if (reply)
                                                *reply = incoming;
                                        else
                                                sd_bus_message_unref(incoming);

                                        return 0;
                                }

                                if (incoming->header->type == SD_BUS_MESSAGE_TYPE_METHOD_ERROR) {
                                        int k;

                                        r = sd_bus_error_copy(error, &incoming->error);
                                        if (r < 0) {
                                                sd_bus_message_unref(incoming);
                                                return r;
                                        }

                                        k = bus_error_to_errno(&incoming->error);
                                        sd_bus_message_unref(incoming);
                                        return k;
                                }

                                sd_bus_message_unref(incoming);
                                return -EIO;
                        }

                        /* There's already guaranteed to be room for
                         * this, so need to resize things here */
                        bus->rqueue[bus->rqueue_size ++] = incoming;
                        room = false;

                        /* Try to read more, right-away */
                        continue;
                }
                if (r != 0)
                        continue;

                if (timeout > 0) {
                        usec_t n;

                        n = now(CLOCK_MONOTONIC);
                        if (n >= timeout)
                                return -ETIMEDOUT;

                        left = timeout - n;
                } else
                        left = (uint64_t) -1;

                r = bus_poll(bus, true, left);
                if (r < 0)
                        return r;

                r = dispatch_wqueue(bus);
                if (r < 0)
                        return r;
        }
}

int sd_bus_get_fd(sd_bus *bus) {
        if (!bus)
                return -EINVAL;
        if (!BUS_IS_OPEN(bus->state))
                return -ENOTCONN;
        if (bus->input_fd != bus->output_fd)
                return -EPERM;
        if (bus_pid_changed(bus))
                return -ECHILD;

        return bus->input_fd;
}

int sd_bus_get_events(sd_bus *bus) {
        int flags = 0;

        if (!bus)
                return -EINVAL;
        if (!BUS_IS_OPEN(bus->state))
                return -ENOTCONN;
        if (bus_pid_changed(bus))
                return -ECHILD;

        if (bus->state == BUS_OPENING)
                flags |= POLLOUT;
        else if (bus->state == BUS_AUTHENTICATING) {

                if (bus_socket_auth_needs_write(bus))
                        flags |= POLLOUT;

                flags |= POLLIN;

        } else if (bus->state == BUS_RUNNING || bus->state == BUS_HELLO) {
                if (bus->rqueue_size <= 0)
                        flags |= POLLIN;
                if (bus->wqueue_size > 0)
                        flags |= POLLOUT;
        }

        return flags;
}

int sd_bus_get_timeout(sd_bus *bus, uint64_t *timeout_usec) {
        struct reply_callback *c;

        if (!bus)
                return -EINVAL;
        if (!timeout_usec)
                return -EINVAL;
        if (!BUS_IS_OPEN(bus->state))
                return -ENOTCONN;
        if (bus_pid_changed(bus))
                return -ECHILD;

        if (bus->state == BUS_AUTHENTICATING) {
                *timeout_usec = bus->auth_timeout;
                return 1;
        }

        if (bus->state != BUS_RUNNING && bus->state != BUS_HELLO) {
                *timeout_usec = (uint64_t) -1;
                return 0;
        }

        c = prioq_peek(bus->reply_callbacks_prioq);
        if (!c) {
                *timeout_usec = (uint64_t) -1;
                return 0;
        }

        *timeout_usec = c->timeout;
        return 1;
}

static int process_timeout(sd_bus *bus) {
        _cleanup_bus_message_unref_ sd_bus_message* m = NULL;
        struct reply_callback *c;
        usec_t n;
        int r;

        assert(bus);

        c = prioq_peek(bus->reply_callbacks_prioq);
        if (!c)
                return 0;

        n = now(CLOCK_MONOTONIC);
        if (c->timeout > n)
                return 0;

        r = bus_message_new_synthetic_error(
                        bus,
                        c->serial,
                        &SD_BUS_ERROR_MAKE("org.freedesktop.DBus.Error.Timeout", "Timed out"),
                        &m);
        if (r < 0)
                return r;

        assert_se(prioq_pop(bus->reply_callbacks_prioq) == c);
        hashmap_remove(bus->reply_callbacks, &c->serial);

        r = c->callback(bus, m, c->userdata);
        free(c);

        return r < 0 ? r : 1;
}

static int process_hello(sd_bus *bus, sd_bus_message *m) {
        assert(bus);
        assert(m);

        if (bus->state != BUS_HELLO)
                return 0;

        /* Let's make sure the first message on the bus is the HELLO
         * reply. But note that we don't actually parse the message
         * here (we leave that to the usual handling), we just verify
         * we don't let any earlier msg through. */

        if (m->header->type != SD_BUS_MESSAGE_TYPE_METHOD_RETURN &&
            m->header->type != SD_BUS_MESSAGE_TYPE_METHOD_ERROR)
                return -EIO;

        if (m->reply_serial != bus->hello_serial)
                return -EIO;

        return 0;
}

static int process_reply(sd_bus *bus, sd_bus_message *m) {
        struct reply_callback *c;
        int r;

        assert(bus);
        assert(m);

        if (m->header->type != SD_BUS_MESSAGE_TYPE_METHOD_RETURN &&
            m->header->type != SD_BUS_MESSAGE_TYPE_METHOD_ERROR)
                return 0;

        c = hashmap_remove(bus->reply_callbacks, &m->reply_serial);
        if (!c)
                return 0;

        if (c->timeout != 0)
                prioq_remove(bus->reply_callbacks_prioq, c, &c->prioq_idx);

        r = sd_bus_message_rewind(m, true);
        if (r < 0)
                return r;

        r = c->callback(bus, m, c->userdata);
        free(c);

        return r;
}

static int process_filter(sd_bus *bus, sd_bus_message *m) {
        struct filter_callback *l;
        int r;

        assert(bus);
        assert(m);

        do {
                bus->filter_callbacks_modified = false;

                LIST_FOREACH(callbacks, l, bus->filter_callbacks) {

                        if (bus->filter_callbacks_modified)
                                break;

                        /* Don't run this more than once per iteration */
                        if (l->last_iteration == bus->iteration_counter)
                                continue;

                        l->last_iteration = bus->iteration_counter;

                        r = sd_bus_message_rewind(m, true);
                        if (r < 0)
                                return r;

                        r = l->callback(bus, m, l->userdata);
                        if (r != 0)
                                return r;

                }

        } while (bus->filter_callbacks_modified);

        return 0;
}

static int process_match(sd_bus *bus, sd_bus_message *m) {
        int r;

        assert(bus);
        assert(m);

        do {
                bus->match_callbacks_modified = false;

                r = bus_match_run(bus, &bus->match_callbacks, m);
                if (r != 0)
                        return r;

        } while (bus->match_callbacks_modified);

        return 0;
}

static int process_builtin(sd_bus *bus, sd_bus_message *m) {
        _cleanup_bus_message_unref_ sd_bus_message *reply = NULL;
        int r;

        assert(bus);
        assert(m);

        if (m->header->type != SD_BUS_MESSAGE_TYPE_METHOD_CALL)
                return 0;

        if (!streq_ptr(m->interface, "org.freedesktop.DBus.Peer"))
                return 0;

        if (m->header->flags & SD_BUS_MESSAGE_NO_REPLY_EXPECTED)
                return 1;

        if (streq_ptr(m->member, "Ping"))
                r = sd_bus_message_new_method_return(bus, m, &reply);
        else if (streq_ptr(m->member, "GetMachineId")) {
                sd_id128_t id;
                char sid[33];

                r = sd_id128_get_machine(&id);
                if (r < 0)
                        return r;

                r = sd_bus_message_new_method_return(bus, m, &reply);
                if (r < 0)
                        return r;

                r = sd_bus_message_append(reply, "s", sd_id128_to_string(id, sid));
        } else {
                r = sd_bus_message_new_method_errorf(
                                bus, m, &reply,
                                "org.freedesktop.DBus.Error.UnknownMethod",
                                 "Unknown method '%s' on interface '%s'.", m->member, m->interface);
        }

        if (r < 0)
                return r;

        r = sd_bus_send(bus, reply, NULL);
        if (r < 0)
                return r;

        return 1;
}

static int node_vtable_get_userdata(
                sd_bus *bus,
                const char *path,
                struct node_vtable *c,
                void **userdata) {

        void *u;
        int r;

        assert(bus);
        assert(path);
        assert(c);

        u = c->userdata;
        if (c->find) {
                r = c->find(bus, path, c->interface, &u, u);
                if (r <= 0)
                        return r;
        }

        if (userdata)
                *userdata = u;

        return 1;
}

static void *vtable_property_convert_userdata(const sd_bus_vtable *p, void *u) {
        assert(p);

        return (uint8_t*) u + p->property.offset;
}

static int vtable_property_get_userdata(
                sd_bus *bus,
                const char *path,
                struct vtable_member *p,
                void **userdata) {

        void *u;
        int r;

        assert(bus);
        assert(path);
        assert(p);
        assert(userdata);

        r = node_vtable_get_userdata(bus, path, p->parent, &u);
        if (r <= 0)
                return r;

        *userdata = vtable_property_convert_userdata(p->vtable, u);
        return 1;
}

static int add_enumerated_to_set(sd_bus *bus, const char *prefix, struct node_enumerator *first, Set *s) {
        struct node_enumerator *c;
        int r;

        assert(bus);
        assert(prefix);
        assert(s);

        LIST_FOREACH(enumerators, c, first) {
                char **children = NULL, **k;

                r = c->callback(bus, prefix, &children, c->userdata);
                if (r < 0)
                        return r;

                STRV_FOREACH(k, children) {
                        if (r < 0) {
                                free(*k);
                                continue;
                        }

                        if (!object_path_is_valid(*k) && object_path_startswith(*k, prefix)) {
                                free(*k);
                                r = -EINVAL;
                                continue;
                        }

                        r = set_consume(s, *k);
                }

                free(children);
                if (r < 0)
                        return r;
        }

        return 0;
}

static int add_subtree_to_set(sd_bus *bus, const char *prefix, struct node *n, Set *s) {
        struct node *i;
        int r;

        assert(bus);
        assert(prefix);
        assert(n);
        assert(s);

        r = add_enumerated_to_set(bus, prefix, n->enumerators, s);
        if (r < 0)
                return r;

        LIST_FOREACH(siblings, i, n->child) {
                char *t;

                t = strdup(i->path);
                if (!t)
                        return -ENOMEM;

                r = set_consume(s, t);
                if (r < 0 && r != -EEXIST)
                        return r;

                r = add_subtree_to_set(bus, prefix, i, s);
                if (r < 0)
                        return r;
        }

        return 0;
}

static int get_child_nodes(sd_bus *bus, const char *prefix, struct node *n, Set **_s) {
        Set *s = NULL;
        int r;

        assert(bus);
        assert(n);
        assert(_s);

        s = set_new(string_hash_func, string_compare_func);
        if (!s)
                return -ENOMEM;

        r = add_subtree_to_set(bus, prefix, n, s);
        if (r < 0) {
                set_free_free(s);
                return r;
        }

        *_s = s;
        return 0;
}

static int node_callbacks_run(
                sd_bus *bus,
                sd_bus_message *m,
                struct node_callback *first,
                bool require_fallback,
                bool *found_object) {

        struct node_callback *c;
        int r;

        assert(bus);
        assert(m);
        assert(found_object);

        LIST_FOREACH(callbacks, c, first) {
                if (require_fallback && !c->is_fallback)
                        continue;

                *found_object = true;

                if (c->last_iteration == bus->iteration_counter)
                        continue;

                r = sd_bus_message_rewind(m, true);
                if (r < 0)
                        return r;

                r = c->callback(bus, m, c->userdata);
                if (r != 0)
                        return r;
        }

        return 0;
}

static int method_callbacks_run(
                sd_bus *bus,
                sd_bus_message *m,
                struct vtable_member *c,
                bool require_fallback,
                bool *found_object) {

        const char *signature;
        void *u;
        int r;

        assert(bus);
        assert(m);
        assert(c);
        assert(found_object);

        if (require_fallback && !c->parent->is_fallback)
                return 0;

        r = node_vtable_get_userdata(bus, m->path, c->parent, &u);
        if (r <= 0)
                return r;

        *found_object = true;

        r = sd_bus_message_rewind(m, true);
        if (r < 0)
                return r;

        r = sd_bus_message_get_signature(m, true, &signature);
        if (r < 0)
                return r;

        if (!streq(c->vtable->method.signature, signature)) {
                r = sd_bus_reply_method_errorf(bus, m,
                                               "org.freedesktop.DBus.Error.InvalidArgs",
                                               "Invalid arguments '%s' to call %s:%s, expecting '%s'.",
                                               signature, c->interface, c->member, c->vtable->method.signature);
                if (r < 0)
                        return r;

                return 1;
        }

        return c->vtable->method.handler(bus, m, u);
}

static int property_get_set_callbacks_run(
                sd_bus *bus,
                sd_bus_message *m,
                struct vtable_member *c,
                bool require_fallback,
                bool is_get,
                bool *found_object) {

        _cleanup_bus_message_unref_ sd_bus_message *reply = NULL;
        _cleanup_bus_error_free_ sd_bus_error error = SD_BUS_ERROR_NULL;
        void *u;
        int r;

        assert(bus);
        assert(m);
        assert(found_object);

        if (require_fallback && !c->parent->is_fallback)
                return 0;

        r = vtable_property_get_userdata(bus, m->path, c, &u);
        if (r <= 0)
                return r;

        *found_object = true;

        r = sd_bus_message_new_method_return(bus, m, &reply);
        if (r < 0)
                return r;

        c->last_iteration = bus->iteration_counter;

        if (is_get) {
                r = sd_bus_message_open_container(reply, 'v', c->vtable->property.signature);
                if (r < 0)
                        return r;

                if (c->vtable->property.get) {
                        r = c->vtable->property.get(bus, m->path, c->interface, c->member, reply, &error, u);
                        if (r < 0)
                                return r;
                } else
                        assert_not_reached("automatic properties not supported yet");

                if (sd_bus_error_is_set(&error)) {
                        r = sd_bus_reply_method_error(bus, m, &error);
                        if (r < 0)
                                return r;

                        return 1;
                }

                r = sd_bus_message_close_container(reply);
                if (r < 0)
                        return r;

        } else {
                if (c->vtable->type != _SD_BUS_VTABLE_WRITABLE_PROPERTY)
                        sd_bus_error_setf(&error, "org.freedesktop.DBus.Error.PropertyReadOnly", "Property '%s' is not writable.", c->member);
                else  {
                        r = sd_bus_message_enter_container(m, 'v', c->vtable->property.signature);
                        if (r < 0)
                                return r;

                        if (c->vtable->property.set) {
                                r = c->vtable->property.set(bus, m->path, c->interface, c->member, m, &error, u);
                                if (r < 0)
                                        return r;
                        } else
                                assert_not_reached("automatic properties not supported yet");
                }

                if (sd_bus_error_is_set(&error)) {
                        r = sd_bus_reply_method_error(bus, m, &error);
                        if (r < 0)
                                return r;

                        return 1;
                }

                r = sd_bus_message_exit_container(m);
                if (r < 0)
                        return r;
        }

        r = sd_bus_send(bus, reply, NULL);
        if (r < 0)
                return r;

        return 1;
}

static int vtable_append_all_properties(
                sd_bus *bus,
                sd_bus_message *reply,
                const char *path,
                struct node_vtable *c,
                void *userdata,
                sd_bus_error *error) {

        const sd_bus_vtable *v;
        int r;

        assert(bus);
        assert(reply);
        assert(c);

        for (v = c->vtable+1; v->type != _SD_BUS_VTABLE_END; v++) {
                if (v->type != _SD_BUS_VTABLE_PROPERTY && v->type != _SD_BUS_VTABLE_WRITABLE_PROPERTY)
                        continue;

                r = sd_bus_message_open_container(reply, 'e', "sv");
                if (r < 0)
                        return r;

                r = sd_bus_message_append(reply, "s", c->interface);
                if (r < 0)
                        return r;

                r = sd_bus_message_open_container(reply, 'v', v->property.signature);
                if (r < 0)
                        return r;

                r = v->property.get(bus, path, c->interface, v->property.member, reply, error, vtable_property_convert_userdata(v, userdata));
                if (r < 0)
                        return r;

                if (sd_bus_error_is_set(error))
                        return 0;

                r = sd_bus_message_close_container(reply);
                if (r < 0)
                        return r;

                r = sd_bus_message_close_container(reply);
                if (r < 0)
                        return r;
        }

        return 1;
}

static int property_get_all_callbacks_run(
                sd_bus *bus,
                sd_bus_message *m,
                struct node_vtable *first,
                bool require_fallback,
                const char *iface,
                bool *found_object) {

        _cleanup_bus_message_unref_ sd_bus_message *reply = NULL;
        struct node_vtable *c;
        bool found_interface = false;
        int r;

        assert(bus);
        assert(m);
        assert(found_object);

        r = sd_bus_message_new_method_return(bus, m, &reply);
        if (r < 0)
                return r;

        r = sd_bus_message_open_container(reply, 'a', "{sv}");
        if (r < 0)
                return r;

        LIST_FOREACH(vtables, c, first) {
                _cleanup_bus_error_free_ sd_bus_error error = SD_BUS_ERROR_NULL;
                void *u;

                if (require_fallback && !c->is_fallback)
                        continue;

                r = node_vtable_get_userdata(bus, m->path, c, &u);
                if (r < 0)
                        return r;
                if (r == 0)
                        continue;

                *found_object = true;

                if (iface && !streq(c->interface, iface))
                        continue;
                found_interface = true;

                c->last_iteration = bus->iteration_counter;

                r = vtable_append_all_properties(bus, reply, m->path, c, u, &error);
                if (r < 0)
                        return r;

                if (sd_bus_error_is_set(&error)) {
                        r = sd_bus_reply_method_error(bus, m, &error);
                        if (r < 0)
                                return r;

                        return 1;
                }
        }

        if (!found_interface) {
                r = sd_bus_reply_method_errorf(
                                bus, m,
                                "org.freedesktop.DBus.Error.UnknownInterface",
                                "Unknown interface '%s'.", iface);
                if (r < 0)
                        return r;

                return 1;
        }

        r = sd_bus_message_close_container(reply);
        if (r < 0)
                return r;

        r = sd_bus_send(bus, reply, NULL);
        if (r < 0)
                return r;

        return 1;
}

static bool bus_node_with_object_manager(sd_bus *bus, struct node *n) {
        assert(bus);

        if (n->object_manager)
                return true;

        if (n->parent)
                return bus_node_with_object_manager(bus, n->parent);

        return false;
}

static bool bus_node_exists(sd_bus *bus, struct node *n, const char *path, bool require_fallback) {
        struct node_vtable *c;
        struct node_callback *k;

        assert(bus);
        assert(n);

        /* Tests if there's anything attached directly to this node
         * for the specified path */

        LIST_FOREACH(callbacks, k, n->callbacks) {
                if (require_fallback && !k->is_fallback)
                        continue;

                return true;
        }

        LIST_FOREACH(vtables, c, n->vtables) {

                if (require_fallback && !c->is_fallback)
                        continue;

                if (node_vtable_get_userdata(bus, path, c, NULL) > 0)
                        return true;
        }

        return !require_fallback && (n->enumerators || n->object_manager);
}

static int process_introspect(
                sd_bus *bus,
                sd_bus_message *m,
                struct node *n,
                bool require_fallback,
                bool *found_object) {

        _cleanup_bus_message_unref_ sd_bus_message *reply = NULL;
        _cleanup_set_free_free_ Set *s = NULL;
        struct introspect intro;
        struct node_vtable *c;
        bool empty;
        int r;

        assert(bus);
        assert(m);
        assert(n);
        assert(found_object);

        r = get_child_nodes(bus, m->path, n, &s);
        if (r < 0)
                return r;

        r = introspect_begin(&intro);
        if (r < 0)
                return r;

        r = introspect_write_default_interfaces(&intro, bus_node_with_object_manager(bus, n));
        if (r < 0)
                return r;

        empty = set_isempty(s);

        LIST_FOREACH(vtables, c, n->vtables) {
                if (require_fallback && !c->is_fallback)
                        continue;

                r = node_vtable_get_userdata(bus, m->path, c, NULL);
                if (r < 0)
                        return r;
                if (r == 0)
                        continue;

                empty = false;

                r = introspect_write_interface(&intro, c->interface, c->vtable);
                if (r < 0)
                        goto finish;
        }

        if (empty) {
                /* Nothing?, let's see if we exist at all, and if not
                 * refuse to do anything */
                r = bus_node_exists(bus, n, m->path, require_fallback);
                if (r < 0)
                        return r;

                if (r == 0)
                        goto finish;
        }

        *found_object = true;

        r = introspect_write_child_nodes(&intro, s, m->path);
        if (r < 0)
                goto finish;

        r = introspect_finish(&intro, bus, m, &reply);
        if (r < 0)
                goto finish;

        r = sd_bus_send(bus, reply, NULL);
        if (r < 0)
                goto finish;

        r = 1;

finish:
        introspect_free(&intro);
        return r;
}

static int object_manager_serialize_vtable(
                sd_bus *bus,
                sd_bus_message *reply,
                const char *path,
                struct node_vtable *c,
                sd_bus_error *error) {

        void *u;
        int r;

        assert(bus);
        assert(reply);
        assert(path);
        assert(c);
        assert(error);

        r = node_vtable_get_userdata(bus, path, c, &u);
        if (r <= 0)
                return r;

        r = sd_bus_message_open_container(reply, 'e', "sa{sv}");
        if (r < 0)
                return r;

        r = sd_bus_message_append(reply, "s", c->interface);
        if (r < 0)
                return r;

        r = sd_bus_message_open_container(reply, 'a', "{sv}");
        if (r < 0)
                return r;

        r = vtable_append_all_properties(bus, reply, path, c, u, error);
        if (r < 0)
                return r;

        r = sd_bus_message_close_container(reply);
        if (r < 0)
                return r;

        r = sd_bus_message_close_container(reply);
        if (r < 0)
                return r;

        return 0;
}

static int object_manager_serialize_path(
                sd_bus *bus,
                sd_bus_message *reply,
                const char *prefix,
                const char *path,
                bool require_fallback,
                sd_bus_error *error) {

        struct node_vtable *i;
        struct node *n;
        int r;

        assert(bus);
        assert(reply);
        assert(prefix);
        assert(path);
        assert(error);

        n = hashmap_get(bus->nodes, prefix);
        if (!n)
                return 0;

        r = sd_bus_message_open_container(reply, 'e', "oa{sa{sv}}");
        if (r < 0)
                return r;

        r = sd_bus_message_append(reply, "o", path);
        if (r < 0)
                return r;

        r = sd_bus_message_open_container(reply, 'a', "{sa{sv}}");
        if (r < 0)
                return r;

        LIST_FOREACH(vtables, i, n->vtables) {

                if (require_fallback && !i->is_fallback)
                        continue;

                r = object_manager_serialize_vtable(bus, reply, path, i, error);
                if (r < 0)
                        return r;
                if (sd_bus_error_is_set(error))
                        return 0;
        }

        r = sd_bus_message_close_container(reply);
        if (r < 0)
                return r;

        r = sd_bus_message_close_container(reply);
        if (r < 0)
                return r;

        return 1;
}

static int object_manager_serialize_path_and_fallbacks(
                sd_bus *bus,
                sd_bus_message *reply,
                const char *path,
                sd_bus_error *error) {

        size_t pl;
        int r;

        assert(bus);
        assert(reply);
        assert(path);
        assert(error);

        /* First, add all vtables registered for this path */
        r = object_manager_serialize_path(bus, reply, path, path, false, error);
        if (r < 0)
                return r;
        if (sd_bus_error_is_set(error))
                return 0;

        /* Second, add fallback vtables registered for any of the prefixes */
        pl = strlen(path);
        if (pl > 1) {
                char p[pl + 1];
                strcpy(p, path);

                for (;;) {
                        char *e;

                        e = strrchr(p, '/');
                        if (e == p || !e)
                                break;

                        *e = 0;

                        r = object_manager_serialize_path(bus, reply, p, path, true, error);
                        if (r < 0)
                                return r;

                        if (sd_bus_error_is_set(error))
                                return 0;
                }
        }

        return 0;
}

static int process_get_managed_objects(
                sd_bus *bus,
                sd_bus_message *m,
                struct node *n,
                bool require_fallback,
                bool *found_object) {

        _cleanup_bus_message_unref_ sd_bus_message *reply = NULL;
        _cleanup_set_free_free_ Set *s = NULL;
        bool empty;
        int r;

        assert(bus);
        assert(m);
        assert(n);
        assert(found_object);

        if (!bus_node_with_object_manager(bus, n))
                return 0;

        r = get_child_nodes(bus, m->path, n, &s);
        if (r < 0)
                return r;

        r = sd_bus_message_new_method_return(bus, m, &reply);
        if (r < 0)
                return r;

        r = sd_bus_message_open_container(reply, 'a', "{oa{sa{sv}}}");
        if (r < 0)
                return r;

        empty = set_isempty(s);
        if (empty) {
                struct node_vtable *c;

                /* Hmm, so we have no children? Then let's check
                 * whether we exist at all, i.e. whether at least one
                 * vtable exists. */

                LIST_FOREACH(vtables, c, n->vtables) {

                        if (require_fallback && !c->is_fallback)
                                continue;

                        if (r < 0)
                                return r;
                        if (r == 0)
                                continue;

                        empty = false;
                        break;
                }

                if (empty)
                        return 0;
        } else {
                Iterator i;
                char *path;

                SET_FOREACH(path, s, i) {
                        _cleanup_bus_error_free_ sd_bus_error error = SD_BUS_ERROR_NULL;

                        r = object_manager_serialize_path_and_fallbacks(bus, reply, path, &error);
                        if (r < 0)
                                return -ENOMEM;

                        if (sd_bus_error_is_set(&error)) {
                                r = sd_bus_reply_method_error(bus, m, &error);
                                if (r < 0)
                                        return r;

                                return 1;
                        }
                }
        }

        r = sd_bus_message_close_container(reply);
        if (r < 0)
                return r;

        r = sd_bus_send(bus, reply, NULL);
        if (r < 0)
                return r;

        return 1;
}

static int object_find_and_run(
                sd_bus *bus,
                sd_bus_message *m,
                const char *p,
                bool require_fallback,
                bool *found_object) {

        struct node *n;
        struct vtable_member vtable_key, *v;
        int r;

        assert(bus);
        assert(m);
        assert(p);
        assert(found_object);

        n = hashmap_get(bus->nodes, p);
        if (!n)
                return 0;

        /* First, try object callbacks */
        r = node_callbacks_run(bus, m, n->callbacks, require_fallback, found_object);
        if (r != 0)
                return r;

        if (!m->interface || !m->member)
                return 0;

        /* Then, look for a known method */
        vtable_key.path = (char*) p;
        vtable_key.interface = m->interface;
        vtable_key.member = m->member;

        v = hashmap_get(bus->vtable_methods, &vtable_key);
        if (v) {
                r = method_callbacks_run(bus, m, v, require_fallback, found_object);
                if (r != 0)
                        return r;
        }

        /* Then, look for a known property */
        if (streq(m->interface, "org.freedesktop.DBus.Properties")) {
                bool get = false;

                get = streq(m->member, "Get");

                if (get || streq(m->member, "Set")) {

                        r = sd_bus_message_rewind(m, true);
                        if (r < 0)
                                return r;

                        vtable_key.path = (char*) p;

                        r = sd_bus_message_read(m, "ss", &vtable_key.interface, &vtable_key.member);
                        if (r < 0)
                                return r;

                        v = hashmap_get(bus->vtable_properties, &vtable_key);
                        if (v) {
                                r = property_get_set_callbacks_run(bus, m, v, require_fallback, get, found_object);
                                if (r != 0)
                                        return r;
                        }

                } else if (streq(m->member, "GetAll")) {
                        const char *iface;

                        r = sd_bus_message_rewind(m, true);
                        if (r < 0)
                                return r;

                        r = sd_bus_message_read(m, "s", &iface);
                        if (r < 0)
                                return r;

                        if (iface[0] == 0)
                                iface = NULL;

                        r = property_get_all_callbacks_run(bus, m, n->vtables, require_fallback, iface, found_object);
                        if (r != 0)
                                return r;
                }

        } else if (sd_bus_message_is_method_call(m, "org.freedesktop.DBus.Introspectable", "Introspect")) {

                r = process_introspect(bus, m, n, require_fallback, found_object);
                if (r != 0)
                        return r;

        } else if (sd_bus_message_is_method_call(m, "org.freedesktop.DBus.ObjectManager", "GetManagedObjects")) {

                r = process_get_managed_objects(bus, m, n, require_fallback, found_object);
                if (r != 0)
                        return r;
        }

        if (!*found_object) {
                r = bus_node_exists(bus, n, m->path, require_fallback);
                if (r < 0)
                        return r;

                if (r > 0)
                        *found_object = true;
        }

        return 0;
}

static int process_object(sd_bus *bus, sd_bus_message *m) {
        int r;
        size_t pl;
        bool found_object = false;

        assert(bus);
        assert(m);

        if (m->header->type != SD_BUS_MESSAGE_TYPE_METHOD_CALL)
                return 0;

        if (!m->path)
                return 0;

        if (hashmap_isempty(bus->nodes))
                return 0;

        pl = strlen(m->path);
        do {
                char p[pl+1];

                bus->nodes_modified = false;

                r = object_find_and_run(bus, m, m->path, false, &found_object);
                if (r != 0)
                        return r;

                /* Look for fallback prefixes */
                strcpy(p, m->path);
                for (;;) {
                        char *e;

                        if (streq(p, "/"))
                                break;

                        if (bus->nodes_modified)
                                break;

                        e = strrchr(p, '/');
                        assert(e);
                        if (e == p)
                                *(e+1) = 0;
                        else
                                *e = 0;

                        r = object_find_and_run(bus, m, p, true, &found_object);
                        if (r != 0)
                                return r;
                }

        } while (bus->nodes_modified);

        if (!found_object)
                return 0;

        if (sd_bus_message_is_method_call(m, "org.freedesktop.DBus.Properties", "Get") ||
            sd_bus_message_is_method_call(m, "org.freedesktop.DBus.Properties", "Set"))
                r = sd_bus_reply_method_errorf(
                                bus, m,
                                "org.freedesktop.DBus.Error.UnknownProperty",
                                "Unknown property or interface.");
        else
                r = sd_bus_reply_method_errorf(
                                bus, m,
                                "org.freedesktop.DBus.Error.UnknownMethod",
                                "Unknown method '%s' or interface '%s'.", m->member, m->interface);

        if (r < 0)
                return r;

        return 1;
}

static int process_message(sd_bus *bus, sd_bus_message *m) {
        int r;

        assert(bus);
        assert(m);

        bus->iteration_counter++;

        r = process_hello(bus, m);
        if (r != 0)
                return r;

        r = process_reply(bus, m);
        if (r != 0)
                return r;

        r = process_filter(bus, m);
        if (r != 0)
                return r;

        r = process_match(bus, m);
        if (r != 0)
                return r;

        r = process_builtin(bus, m);
        if (r != 0)
                return r;

        return process_object(bus, m);
}

static int process_running(sd_bus *bus, sd_bus_message **ret) {
        _cleanup_bus_message_unref_ sd_bus_message *m = NULL;
        int r;

        assert(bus);
        assert(bus->state == BUS_RUNNING || bus->state == BUS_HELLO);

        r = process_timeout(bus);
        if (r != 0)
                goto null_message;

        r = dispatch_wqueue(bus);
        if (r != 0)
                goto null_message;

        r = dispatch_rqueue(bus, &m);
        if (r < 0)
                return r;
        if (!m)
                goto null_message;

        r = process_message(bus, m);
        if (r != 0)
                goto null_message;

        if (ret) {
                r = sd_bus_message_rewind(m, true);
                if (r < 0)
                        return r;

                *ret = m;
                m = NULL;
                return 1;
        }

        if (m->header->type == SD_BUS_MESSAGE_TYPE_METHOD_CALL) {

                r = sd_bus_reply_method_errorf(
                                bus, m,
                                "org.freedesktop.DBus.Error.UnknownObject",
                                "Unknown object '%s'.", m->path);
                if (r < 0)
                        return r;
        }

        return 1;

null_message:
        if (r >= 0 && ret)
                *ret = NULL;

        return r;
}

int sd_bus_process(sd_bus *bus, sd_bus_message **ret) {
        int r;

        /* Returns 0 when we didn't do anything. This should cause the
         * caller to invoke sd_bus_wait() before returning the next
         * time. Returns > 0 when we did something, which possibly
         * means *ret is filled in with an unprocessed message. */

        if (!bus)
                return -EINVAL;
        if (bus_pid_changed(bus))
                return -ECHILD;

        /* We don't allow recursively invoking sd_bus_process(). */
        if (bus->processing)
                return -EBUSY;

        switch (bus->state) {

        case BUS_UNSET:
        case BUS_CLOSED:
                return -ENOTCONN;

        case BUS_OPENING:
                r = bus_socket_process_opening(bus);
                if (r < 0)
                        return r;
                if (ret)
                        *ret = NULL;
                return r;

        case BUS_AUTHENTICATING:

                r = bus_socket_process_authenticating(bus);
                if (r < 0)
                        return r;
                if (ret)
                        *ret = NULL;
                return r;

        case BUS_RUNNING:
        case BUS_HELLO:

                bus->processing = true;
                r = process_running(bus, ret);
                bus->processing = false;

                return r;
        }

        assert_not_reached("Unknown state");
}

static int bus_poll(sd_bus *bus, bool need_more, uint64_t timeout_usec) {
        struct pollfd p[2] = {};
        int r, e, n;
        struct timespec ts;
        usec_t until, m;

        assert(bus);

        if (!BUS_IS_OPEN(bus->state))
                return -ENOTCONN;

        e = sd_bus_get_events(bus);
        if (e < 0)
                return e;

        if (need_more)
                e |= POLLIN;

        r = sd_bus_get_timeout(bus, &until);
        if (r < 0)
                return r;
        if (r == 0)
                m = (uint64_t) -1;
        else {
                usec_t nw;
                nw = now(CLOCK_MONOTONIC);
                m = until > nw ? until - nw : 0;
        }

        if (timeout_usec != (uint64_t) -1 && (m == (uint64_t) -1 || timeout_usec < m))
                m = timeout_usec;

        p[0].fd = bus->input_fd;
        if (bus->output_fd == bus->input_fd) {
                p[0].events = e;
                n = 1;
        } else {
                p[0].events = e & POLLIN;
                p[1].fd = bus->output_fd;
                p[1].events = e & POLLOUT;
                n = 2;
        }

        r = ppoll(p, n, m == (uint64_t) -1 ? NULL : timespec_store(&ts, m), NULL);
        if (r < 0)
                return -errno;

        return r > 0 ? 1 : 0;
}

int sd_bus_wait(sd_bus *bus, uint64_t timeout_usec) {

        if (!bus)
                return -EINVAL;
        if (!BUS_IS_OPEN(bus->state))
                return -ENOTCONN;
        if (bus_pid_changed(bus))
                return -ECHILD;

        if (bus->rqueue_size > 0)
                return 0;

        return bus_poll(bus, false, timeout_usec);
}

int sd_bus_flush(sd_bus *bus) {
        int r;

        if (!bus)
                return -EINVAL;
        if (!BUS_IS_OPEN(bus->state))
                return -ENOTCONN;
        if (bus_pid_changed(bus))
                return -ECHILD;

        r = bus_ensure_running(bus);
        if (r < 0)
                return r;

        if (bus->wqueue_size <= 0)
                return 0;

        for (;;) {
                r = dispatch_wqueue(bus);
                if (r < 0)
                        return r;

                if (bus->wqueue_size <= 0)
                        return 0;

                r = bus_poll(bus, false, (uint64_t) -1);
                if (r < 0)
                        return r;
        }
}

int sd_bus_add_filter(sd_bus *bus, sd_bus_message_handler_t callback, void *userdata) {
        struct filter_callback *f;

        if (!bus)
                return -EINVAL;
        if (!callback)
                return -EINVAL;
        if (bus_pid_changed(bus))
                return -ECHILD;

        f = new0(struct filter_callback, 1);
        if (!f)
                return -ENOMEM;
        f->callback = callback;
        f->userdata = userdata;

        bus->filter_callbacks_modified = true;
        LIST_PREPEND(struct filter_callback, callbacks, bus->filter_callbacks, f);
        return 0;
}

int sd_bus_remove_filter(sd_bus *bus, sd_bus_message_handler_t callback, void *userdata) {
        struct filter_callback *f;

        if (!bus)
                return -EINVAL;
        if (!callback)
                return -EINVAL;
        if (bus_pid_changed(bus))
                return -ECHILD;

        LIST_FOREACH(callbacks, f, bus->filter_callbacks) {
                if (f->callback == callback && f->userdata == userdata) {
                        bus->filter_callbacks_modified = true;
                        LIST_REMOVE(struct filter_callback, callbacks, bus->filter_callbacks, f);
                        free(f);
                        return 1;
                }
        }

        return 0;
}

static struct node *bus_node_allocate(sd_bus *bus, const char *path) {
        struct node *n, *parent;
        const char *e;
        char *s, *p;
        int r;

        assert(bus);
        assert(path);
        assert(path[0] == '/');

        n = hashmap_get(bus->nodes, path);
        if (n)
                return n;

        r = hashmap_ensure_allocated(&bus->nodes, string_hash_func, string_compare_func);
        if (r < 0)
                return NULL;

        s = strdup(path);
        if (!s)
                return NULL;

        if (streq(path, "/"))
                parent = NULL;
        else {
                e = strrchr(path, '/');
                assert(e);

                p = strndupa(path, MAX(1, path - e));

                parent = bus_node_allocate(bus, p);
                if (!parent) {
                        free(s);
                        return NULL;
                }
        }

        n = new0(struct node, 1);
        if (!n)
                return NULL;

        n->parent = parent;
        n->path = s;

        r = hashmap_put(bus->nodes, s, n);
        if (r < 0) {
                free(s);
                free(n);
                return NULL;
        }

        if (parent)
                LIST_PREPEND(struct node, siblings, parent->child, n);

        return n;
}

static void bus_node_gc(sd_bus *b, struct node *n) {
        assert(b);

        if (!n)
                return;

        if (n->child ||
            n->callbacks ||
            n->vtables ||
            n->enumerators ||
            n->object_manager)
                return;

        assert(hashmap_remove(b->nodes, n->path) == n);

        if (n->parent)
                LIST_REMOVE(struct node, siblings, n->parent->child, n);

        free(n->path);
        bus_node_gc(b, n->parent);
        free(n);
}

static int bus_add_object(
                sd_bus *b,
                bool fallback,
                const char *path,
                sd_bus_message_handler_t callback,
                void *userdata) {

        struct node_callback *c;
        struct node *n;
        int r;

        if (!b)
                return -EINVAL;
        if (!object_path_is_valid(path))
                return -EINVAL;
        if (!callback)
                return -EINVAL;
        if (bus_pid_changed(b))
                return -ECHILD;

        n = bus_node_allocate(b, path);
        if (!n)
                return -ENOMEM;

        c = new0(struct node_callback, 1);
        if (!c) {
                r = -ENOMEM;
                goto fail;
        }

        c->node = n;
        c->callback = callback;
        c->userdata = userdata;
        c->is_fallback = fallback;

        LIST_PREPEND(struct node_callback, callbacks, n->callbacks, c);
        return 0;

fail:
        free(c);
        bus_node_gc(b, n);
        return r;
}

static int bus_remove_object(
                sd_bus *bus,
                bool fallback,
                const char *path,
                sd_bus_message_handler_t callback,
                void *userdata) {

        struct node_callback *c;
        struct node *n;

        if (!bus)
                return -EINVAL;
        if (!object_path_is_valid(path))
                return -EINVAL;
        if (!callback)
                return -EINVAL;
        if (bus_pid_changed(bus))
                return -ECHILD;

        n = hashmap_get(bus->nodes, path);
        if (!n)
                return 0;

        LIST_FOREACH(callbacks, c, n->callbacks)
                if (c->callback == callback && c->userdata == userdata && c->is_fallback == fallback)
                        break;
        if (!c)
                return 0;

        LIST_REMOVE(struct node_callback, callbacks, n->callbacks, c);
        free(c);

        bus_node_gc(bus, n);

        return 1;
}

int sd_bus_add_object(sd_bus *bus, const char *path, sd_bus_message_handler_t callback, void *userdata) {
        return bus_add_object(bus, false, path, callback, userdata);
}

int sd_bus_remove_object(sd_bus *bus, const char *path, sd_bus_message_handler_t callback, void *userdata) {
        return bus_remove_object(bus, false, path, callback, userdata);
}

int sd_bus_add_fallback(sd_bus *bus, const char *prefix, sd_bus_message_handler_t callback, void *userdata) {
        return bus_add_object(bus, true, prefix, callback, userdata);
}

int sd_bus_remove_fallback(sd_bus *bus, const char *prefix, sd_bus_message_handler_t callback, void *userdata) {
        return bus_remove_object(bus, true, prefix, callback, userdata);
}

int sd_bus_add_match(sd_bus *bus, const char *match, sd_bus_message_handler_t callback, void *userdata) {
        struct bus_match_component *components = NULL;
        unsigned n_components = 0;
        uint64_t cookie = 0;
        int r = 0;

        if (!bus)
                return -EINVAL;
        if (!match)
                return -EINVAL;
        if (bus_pid_changed(bus))
                return -ECHILD;

        r = bus_match_parse(match, &components, &n_components);
        if (r < 0)
                goto finish;

        if (bus->bus_client) {
                cookie = ++bus->match_cookie;

                r = bus_add_match_internal(bus, match, components, n_components, cookie);
                if (r < 0)
                        goto finish;
        }

        bus->match_callbacks_modified = true;
        r = bus_match_add(&bus->match_callbacks, components, n_components, callback, userdata, cookie, NULL);
        if (r < 0) {
                if (bus->bus_client)
                        bus_remove_match_internal(bus, match, cookie);
        }

finish:
        bus_match_parse_free(components, n_components);
        return r;
}

int sd_bus_remove_match(sd_bus *bus, const char *match, sd_bus_message_handler_t callback, void *userdata) {
        struct bus_match_component *components = NULL;
        unsigned n_components = 0;
        int r = 0, q = 0;
        uint64_t cookie = 0;

        if (!bus)
                return -EINVAL;
        if (!match)
                return -EINVAL;
        if (bus_pid_changed(bus))
                return -ECHILD;

        r = bus_match_parse(match, &components, &n_components);
        if (r < 0)
                return r;

        bus->match_callbacks_modified = true;
        r = bus_match_remove(&bus->match_callbacks, components, n_components, callback, userdata, &cookie);

        if (bus->bus_client)
                q = bus_remove_match_internal(bus, match, cookie);

        bus_match_parse_free(components, n_components);

        return r < 0 ? r : q;
}

int sd_bus_emit_signal(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *member,
                const char *types, ...) {

        _cleanup_bus_message_unref_ sd_bus_message *m = NULL;
        int r;

        if (!bus)
                return -EINVAL;
        if (!BUS_IS_OPEN(bus->state))
                return -ENOTCONN;
        if (bus_pid_changed(bus))
                return -ECHILD;

        r = sd_bus_message_new_signal(bus, path, interface, member, &m);
        if (r < 0)
                return r;

        if (!isempty(types)) {
                va_list ap;

                va_start(ap, types);
                r = bus_message_append_ap(m, types, ap);
                va_end(ap);
                if (r < 0)
                        return r;
        }

        return sd_bus_send(bus, m, NULL);
}

int sd_bus_call_method(
                sd_bus *bus,
                const char *destination,
                const char *path,
                const char *interface,
                const char *member,
                sd_bus_error *error,
                sd_bus_message **reply,
                const char *types, ...) {

        _cleanup_bus_message_unref_ sd_bus_message *m = NULL;
        int r;

        if (!bus)

                return -EINVAL;
        if (!BUS_IS_OPEN(bus->state))
                return -ENOTCONN;
        if (bus_pid_changed(bus))
                return -ECHILD;

        r = sd_bus_message_new_method_call(bus, destination, path, interface, member, &m);
        if (r < 0)
                return r;

        if (!isempty(types)) {
                va_list ap;

                va_start(ap, types);
                r = bus_message_append_ap(m, types, ap);
                va_end(ap);
                if (r < 0)
                        return r;
        }

        return sd_bus_send_with_reply_and_block(bus, m, 0, error, reply);
}

int sd_bus_reply_method_return(
                sd_bus *bus,
                sd_bus_message *call,
                const char *types, ...) {

        _cleanup_bus_message_unref_ sd_bus_message *m = NULL;
        int r;

        if (!bus)
                return -EINVAL;
        if (!call)
                return -EINVAL;
        if (!call->sealed)
                return -EPERM;
        if (call->header->type != SD_BUS_MESSAGE_TYPE_METHOD_CALL)
                return -EINVAL;
        if (!BUS_IS_OPEN(bus->state))
                return -ENOTCONN;
        if (bus_pid_changed(bus))
                return -ECHILD;

        if (call->header->flags & SD_BUS_MESSAGE_NO_REPLY_EXPECTED)
                return 0;

        r = sd_bus_message_new_method_return(bus, call, &m);
        if (r < 0)
                return r;

        if (!isempty(types)) {
                va_list ap;

                va_start(ap, types);
                r = bus_message_append_ap(m, types, ap);
                va_end(ap);
                if (r < 0)
                        return r;
        }

        return sd_bus_send(bus, m, NULL);
}

int sd_bus_reply_method_error(
                sd_bus *bus,
                sd_bus_message *call,
                const sd_bus_error *e) {

        _cleanup_bus_message_unref_ sd_bus_message *m = NULL;
        int r;

        if (!bus)
                return -EINVAL;
        if (!call)
                return -EINVAL;
        if (!call->sealed)
                return -EPERM;
        if (call->header->type != SD_BUS_MESSAGE_TYPE_METHOD_CALL)
                return -EINVAL;
        if (!sd_bus_error_is_set(e))
                return -EINVAL;
        if (!BUS_IS_OPEN(bus->state))
                return -ENOTCONN;
        if (bus_pid_changed(bus))
                return -ECHILD;

        if (call->header->flags & SD_BUS_MESSAGE_NO_REPLY_EXPECTED)
                return 0;

        r = sd_bus_message_new_method_error(bus, call, e, &m);
        if (r < 0)
                return r;

        return sd_bus_send(bus, m, NULL);
}

int sd_bus_reply_method_errorf(
                sd_bus *bus,
                sd_bus_message *call,
                const char *name,
                const char *format,
                ...) {

        _cleanup_bus_error_free_ sd_bus_error error = SD_BUS_ERROR_NULL;
        char *n, *m;
        va_list ap;
        int r;

        n = strdup(name);
        if (!n)
                return -ENOMEM;

        if (format) {
                va_start(ap, format);
                r = vasprintf(&m, format, ap);
                va_end(ap);

                if (r < 0) {
                        free(n);
                        return -ENOMEM;
                }
        }

        error.name = n;
        error.message = m;
        error.need_free = true;

        return sd_bus_reply_method_error(bus, call, &error);
}

bool bus_pid_changed(sd_bus *bus) {
        assert(bus);

        /* We don't support people creating a bus connection and
         * keeping it around over a fork(). Let's complain. */

        return bus->original_pid != getpid();
}

static void free_node_vtable(sd_bus *bus, struct node_vtable *w) {
        assert(bus);

        if (!w)
                return;

        if (w->interface && w->node && w->vtable) {
                const sd_bus_vtable *v;

                for (v = w->vtable; v->type != _SD_BUS_VTABLE_END; w++) {
                        struct vtable_member *x = NULL;

                        switch (v->type) {

                        case _SD_BUS_VTABLE_METHOD: {
                                struct vtable_member key;

                                key.path = w->node->path;
                                key.interface = w->interface;
                                key.member = v->method.member;

                                x = hashmap_remove(bus->vtable_methods, &key);
                                break;
                        }

                        case _SD_BUS_VTABLE_PROPERTY:
                        case _SD_BUS_VTABLE_WRITABLE_PROPERTY: {
                                struct vtable_member key;

                                key.path = w->node->path;
                                key.interface = w->interface;
                                key.member = v->property.member;
                                x = hashmap_remove(bus->vtable_properties, &key);
                                break;
                        }}

                        free(x);
                }
        }

        free(w->interface);
        free(w);
}

static unsigned vtable_member_hash_func(const void *a) {
        const struct vtable_member *m = a;

        return
                string_hash_func(m->path) ^
                string_hash_func(m->interface) ^
                string_hash_func(m->member);
}

static int vtable_member_compare_func(const void *a, const void *b) {
        const struct vtable_member *x = a, *y = b;
        int r;

        r = strcmp(x->path, y->path);
        if (r != 0)
                return r;

        r = strcmp(x->interface, y->interface);
        if (r != 0)
                return r;

        return strcmp(x->member, y->member);
}

static int add_object_vtable_internal(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const sd_bus_vtable *vtable,
                bool fallback,
                sd_bus_object_find_t find,
                void *userdata) {

        struct node_vtable *c = NULL, *i;
        const sd_bus_vtable *v;
        struct node *n;
        int r;

        if (!bus)
                return -EINVAL;
        if (!object_path_is_valid(path))
                return -EINVAL;
        if (!interface_name_is_valid(interface))
                return -EINVAL;
        if (!vtable || vtable[0].type != _SD_BUS_VTABLE_START || vtable[0].start.element_size != sizeof(struct sd_bus_vtable))
                return -EINVAL;
        if (bus_pid_changed(bus))
                return -ECHILD;

        r = hashmap_ensure_allocated(&bus->vtable_methods, vtable_member_hash_func, vtable_member_compare_func);
        if (r < 0)
                return r;

        r = hashmap_ensure_allocated(&bus->vtable_properties, vtable_member_hash_func, vtable_member_compare_func);
        if (r < 0)
                return r;

        n = bus_node_allocate(bus, path);
        if (!n)
                return -ENOMEM;

        LIST_FOREACH(vtables, i, n->vtables) {
                if (streq(i->interface, interface)) {
                        r = -EEXIST;
                        goto fail;
                }

                if (i->is_fallback != fallback) {
                        r = -EPROTOTYPE;
                        goto fail;
                }
        }

        c = new0(struct node_vtable, 1);
        if (!c) {
                r = -ENOMEM;
                goto fail;
        }

        c->node = n;
        c->is_fallback = fallback;
        c->vtable = vtable;
        c->userdata = userdata;
        c->find = find;

        c->interface = strdup(interface);
        if (!c->interface) {
                r = -ENOMEM;
                goto fail;
        }

        for (v = c->vtable+1; v->type != _SD_BUS_VTABLE_END; v++) {

                switch (v->type) {

                case _SD_BUS_VTABLE_METHOD: {
                        struct vtable_member *m;

                        if (!member_name_is_valid(v->method.member) ||
                            !signature_is_valid(v->method.signature, false) ||
                            !signature_is_valid(v->method.result, false) ||
                            !v->method.handler ||
                            v->flags & (SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE|SD_BUS_VTABLE_PROPERTY_INVALIDATE_ONLY)) {
                                r = -EINVAL;
                                goto fail;
                        }

                        m = new0(struct vtable_member, 1);
                        if (!m) {
                                r = -ENOMEM;
                                goto fail;
                        }

                        m->parent = c;
                        m->path = n->path;
                        m->interface = c->interface;
                        m->member = v->method.member;
                        m->vtable = v;

                        r = hashmap_put(bus->vtable_methods, m, m);
                        if (r < 0) {
                                free(m);
                                goto fail;
                        }

                        break;
                }

                case _SD_BUS_VTABLE_PROPERTY:
                case _SD_BUS_VTABLE_WRITABLE_PROPERTY: {
                        struct vtable_member *m;

                        if (!member_name_is_valid(v->property.member) ||
                            !signature_is_single(v->property.signature, false) ||
                            v->flags & SD_BUS_VTABLE_METHOD_NO_REPLY ||
                            (v->flags & SD_BUS_VTABLE_PROPERTY_INVALIDATE_ONLY && !(v->flags & SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE))) {
                                r = -EINVAL;
                                goto fail;
                        }


                        m = new0(struct vtable_member, 1);
                        if (!m) {
                                r = -ENOMEM;
                                goto fail;
                        }

                        m->parent = c;
                        m->path = n->path;
                        m->interface = c->interface;
                        m->member = v->property.member;
                        m->vtable = v;

                        r = hashmap_put(bus->vtable_properties, m, m);
                        if (r < 0) {
                                free(m);
                                goto fail;
                        }

                        break;
                }

                case _SD_BUS_VTABLE_SIGNAL:

                        if (!member_name_is_valid(v->signal.member) ||
                            !signature_is_single(v->signal.signature, false)) {
                                r = -EINVAL;
                                goto fail;
                        }

                        break;

                default:
                        r = -EINVAL;
                        goto fail;
                }
        }

        LIST_PREPEND(struct node_vtable, vtables, n->vtables, c);
        return 0;

fail:
        if (c)
                free_node_vtable(bus, c);

        bus_node_gc(bus, n);
        return 0;
}

static int remove_object_vtable_internal(
                sd_bus *bus,
                const char *path,
                const char *interface,
                bool fallback) {

        struct node_vtable *c;
        struct node *n;

        if (!bus)
                return -EINVAL;
        if (!object_path_is_valid(path))
                return -EINVAL;
        if (!interface_name_is_valid(interface))
                return -EINVAL;
        if (bus_pid_changed(bus))
                return -ECHILD;

        n = hashmap_get(bus->nodes, path);
        if (!n)
                return 0;

        LIST_FOREACH(vtables, c, n->vtables)
                if (streq(c->interface, interface) && c->is_fallback == fallback)
                        break;

        if (!c)
                return 0;

        LIST_REMOVE(struct node_vtable, vtables, n->vtables, c);

        free_node_vtable(bus, c);
        return 1;
}

int sd_bus_add_object_vtable(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const sd_bus_vtable *vtable,
                void *userdata) {

        return add_object_vtable_internal(bus, path, interface, vtable, false, NULL, userdata);
}

int sd_bus_remove_object_vtable(
                sd_bus *bus,
                const char *path,
                const char *interface) {

        return remove_object_vtable_internal(bus, path, interface, false);
}

int sd_bus_add_fallback_vtable(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const sd_bus_vtable *vtable,
                sd_bus_object_find_t find,
                void *userdata) {

        return add_object_vtable_internal(bus, path, interface, vtable, true, find, userdata);
}

int sd_bus_remove_fallback_vtable(
                sd_bus *bus,
                const char *path,
                const char *interface) {

        return remove_object_vtable_internal(bus, path, interface, true);
}

int sd_bus_add_node_enumerator(
                sd_bus *bus,
                const char *path,
                sd_bus_node_enumerator_t callback,
                void *userdata) {

        struct node_enumerator *c;
        struct node *n;
        int r;

        if (!bus)
                return -EINVAL;
        if (!object_path_is_valid(path))
                return -EINVAL;
        if (!callback)
                return -EINVAL;
        if (bus_pid_changed(bus))
                return -ECHILD;

        n = bus_node_allocate(bus, path);
        if (!n)
                return -ENOMEM;

        c = new0(struct node_enumerator, 1);
        if (!c) {
                r = -ENOMEM;
                goto fail;
        }

        c->node = n;
        c->callback = callback;
        c->userdata = userdata;

        LIST_PREPEND(struct node_enumerator, enumerators, n->enumerators, c);
        return 0;

fail:
        free(c);
        bus_node_gc(bus, n);
        return r;
}

int sd_bus_remove_node_enumerator(
                sd_bus *bus,
                const char *path,
                sd_bus_node_enumerator_t callback,
                void *userdata) {

        struct node_enumerator *c;
        struct node *n;

        if (!bus)
                return -EINVAL;
        if (!object_path_is_valid(path))
                return -EINVAL;
        if (!callback)
                return -EINVAL;
        if (bus_pid_changed(bus))
                return -ECHILD;

        n = hashmap_get(bus->nodes, path);
        if (!n)
                return 0;

        LIST_FOREACH(enumerators, c, n->enumerators)
                if (c->callback == callback && c->userdata == userdata)
                        break;

        if (!c)
                return 0;

        LIST_REMOVE(struct node_enumerator, enumerators, n->enumerators, c);
        free(c);

        bus_node_gc(bus, n);

        return 1;
}

static int emit_properties_changed_on_interface(
                sd_bus *bus,
                const char *prefix,
                const char *path,
                const char *interface,
                bool require_fallback,
                char **names) {

        _cleanup_bus_message_unref_ sd_bus_message *m = NULL;
        bool has_invalidating = false;
        struct vtable_member key;
        struct node_vtable *c;
        struct node *n;
        char **property;
        void *u = NULL;
        int r;

        assert(bus);
        assert(path);
        assert(interface);

        n = hashmap_get(bus->nodes, prefix);
        if (!n)
                return 0;

        LIST_FOREACH(vtables, c, n->vtables) {
                if (require_fallback && !c->is_fallback)
                        continue;

                if (streq(c->interface, interface))
                        break;

                r = node_vtable_get_userdata(bus, path, c, &u);
                if (r < 0)
                        return r;
                if (r > 0)
                        break;
        }

        if (!c)
                return 0;

        r = sd_bus_message_new_signal(bus, path, "org.freedesktop.DBus", "PropertiesChanged", &m);
        if (r < 0)
                return r;

        r = sd_bus_message_append(m, "s", interface);
        if (r < 0)
                return r;

        r = sd_bus_message_open_container(m, 'a', "{sv}");
        if (r < 0)
                return r;

        key.path = prefix;
        key.interface = interface;

        STRV_FOREACH(property, names) {
                _cleanup_bus_error_free_ sd_bus_error error = SD_BUS_ERROR_NULL;
                struct vtable_member *v;

                key.member = *property;
                v = hashmap_get(bus->vtable_properties, &key);
                if (!v)
                        return -ENOENT;

                assert(c == v->parent);

                if (!(v->vtable->flags & SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE))
                        return -EDOM;
                if (v->vtable->flags & SD_BUS_VTABLE_PROPERTY_INVALIDATE_ONLY) {
                        has_invalidating = true;
                        continue;
                }

                r = sd_bus_message_open_container(m, 'e', "sv");
                if (r < 0)
                        return r;

                r = sd_bus_message_append(m, "s", *n);
                if (r < 0)
                        return r;

                r = sd_bus_message_open_container(m, 'v', v->vtable->property.signature);
                if (r < 0)
                        return r;

                r = v->vtable->property.get(bus, m->path, interface, *property, m, &error, vtable_property_convert_userdata(v->vtable, u));
                if (r < 0)
                        return r;

                if (sd_bus_error_is_set(&error))
                        return bus_error_to_errno(&error);

                r = sd_bus_message_close_container(m);
                if (r < 0)
                        return r;

                r = sd_bus_message_close_container(m);
                if (r < 0)
                        return r;
        }

        r = sd_bus_message_close_container(m);
        if (r < 0)
                return r;

        r = sd_bus_message_open_container(m, 'a', "s");
        if (r < 0)
                return r;

        if (has_invalidating) {
                STRV_FOREACH(property, names) {
                        struct vtable_member *v;

                        key.member = *property;
                        assert_se(v = hashmap_get(bus->vtable_properties, &key));
                        assert(c == v->parent);

                        if (!(v->vtable->flags & SD_BUS_VTABLE_PROPERTY_INVALIDATE_ONLY))
                                continue;

                        r = sd_bus_message_append(m, "s", *property);
                        if (r < 0)
                                return r;
                }
        }

        r = sd_bus_message_close_container(m);
        if (r < 0)
                return r;

        r = sd_bus_send(bus, m, NULL);
        if (r < 0)
                return r;

        return 1;
}

int sd_bus_emit_properties_changed_strv(sd_bus *bus, const char *path, const char *interface, char **names) {
        size_t pl;
        int r;

        if (!bus)
                return -EINVAL;
        if (!object_path_is_valid(path))
                return -EINVAL;
        if (!interface_name_is_valid(interface))
                return -EINVAL;

        r = emit_properties_changed_on_interface(bus, path, path, interface, false, names);
        if (r != 0)
                return r;

        pl = strlen(path);
        if (pl > 1 ) {
                char p[pl+1];

                strcpy(p, path);
                for (;;) {
                        char *e;

                        if (streq(p, "/"))
                                break;

                        e = strrchr(p, '/');
                        assert(e);
                        if (e == p)
                                *(e+1) = 0;
                        else
                                *e = 0;

                        r = emit_properties_changed_on_interface(bus, p, path, interface, true, names);
                        if (r != 0)
                                return r;
                }
        }

        return -ENOENT;
}

int sd_bus_emit_properties_changed(sd_bus *bus, const char *path, const char *interface, const char *name, ...)  {
        _cleanup_strv_free_ char **names = NULL;
        va_list ap;

        va_start(ap, name);
        names = strv_new_ap(name, ap);
        va_end(ap);

        if (!names)
                return -ENOMEM;

        return sd_bus_emit_properties_changed_strv(bus, path, interface, names);
}

int sd_bus_emit_interfaces_added(sd_bus *bus, const char *path, const char *interfaces, ...) {
        return -ENOSYS;
}

int sd_bus_emit_interfaces_removed(sd_bus *bus, const char *path, const char *interfaces, ...) {
        return -ENOSYS;
}

int sd_bus_get_property(
                sd_bus *bus,
                const char *destination,
                const char *path,
                const char *interface,
                const char *member,
                sd_bus_error *error,
                sd_bus_message **reply,
                const char *type) {

        sd_bus_message *rep = NULL;
        int r;

        if (interface && !interface_name_is_valid(interface))
                return -EINVAL;
        if (!member_name_is_valid(member))
                return -EINVAL;
        if (!signature_is_single(type, false))
                return -EINVAL;
        if (!reply)
                return -EINVAL;

        r = sd_bus_call_method(bus, destination, path, "org.freedesktop.DBus.Properties", "Get", error, &rep, "ss", strempty(interface), member);
        if (r < 0)
                return r;

        r = sd_bus_message_enter_container(rep, 'v', type);
        if (r < 0) {
                sd_bus_message_unref(rep);
                return r;
        }

        *reply = rep;
        return 0;
}

int sd_bus_set_property(
                sd_bus *bus,
                const char *destination,
                const char *path,
                const char *interface,
                const char *member,
                sd_bus_error *error,
                const char *type, ...) {

        _cleanup_bus_message_unref_ sd_bus_message *m = NULL;
        va_list ap;
        int r;

        if (interface && !interface_name_is_valid(interface))
                return -EINVAL;
        if (!member_name_is_valid(member))
                return -EINVAL;
        if (!signature_is_single(type, false))
                return -EINVAL;

        r = sd_bus_message_new_method_call(bus, destination, path, "org.freedesktop.DBus.Properties", "Set", &m);
        if (r < 0)
                return r;

        r = sd_bus_message_append(m, "ss", strempty(interface), member);
        if (r < 0)
                return r;

        r = sd_bus_message_open_container(m, 'v', type);
        if (r < 0)
                return r;

        va_start(ap, type);
        r = bus_message_append_ap(m, type, ap);
        va_end(ap);
        if (r < 0)
                return r;

        r = sd_bus_message_close_container(m);
        if (r < 0)
                return r;

        return sd_bus_send_with_reply_and_block(bus, m, 0, error, NULL);
}

int sd_bus_add_object_manager(sd_bus *bus, const char *path) {
        struct node *n;

        if (!bus)
                return -EINVAL;
        if (!object_path_is_valid(path))
                return -EINVAL;
        if (bus_pid_changed(bus))
                return -ECHILD;

        n = bus_node_allocate(bus, path);
        if (!n)
                return -ENOMEM;

        n->object_manager = true;
        return 0;
}

int sd_bus_remove_object_manager(sd_bus *bus, const char *path) {
        struct node *n;

        if (!bus)
                return -EINVAL;
        if (!object_path_is_valid(path))
                return -EINVAL;
        if (bus_pid_changed(bus))
                return -ECHILD;

        n = hashmap_get(bus->nodes, path);
        if (!n)
                return 0;

        if (!n->object_manager)
                return 0;

        n->object_manager = false;
        bus_node_gc(bus, n);
        return 1;
}
