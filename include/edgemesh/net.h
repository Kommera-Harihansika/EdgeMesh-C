#ifndef EDGEMESH_NET_H
#define EDGEMESH_NET_H

#include <stddef.h>
#include <sys/types.h>

int em_make_listener(unsigned short port);
ssize_t em_read_headers(int fd, char *buffer, size_t capacity, int timeout_ms);
int em_write_all(int fd, const char *data, size_t length, size_t *written);
void em_set_send_timeout(int fd, int timeout_ms);

#endif
