// Error numbers: the Linux values, as the system calls return them negated (docs/rv.md section 4).
#pragma once

extern int errno;

#define EPERM 1
#define ENOENT 2
#define EIO 5
#define EBADF 9
#define ENOMEM 12
#define EFAULT 14
#define EINVAL 22
#define EDOM 33
#define ERANGE 34
#define ENOSYS 38
