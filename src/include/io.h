/* Copyright (C) NIOVA Systems, Inc - All Rights Reserved
 * Unauthorized copying of this file, via any medium is strictly prohibited
 * Proprietary and confidential
 * Written by Paul Nowoczynski <pauln@niova.io> 2019
 */

#ifndef _NIOVA_IO_H_
#define _NIOVA_IO_H_ 1

ssize_t
io_read(int fd, char *buf, size_t size);

#endif