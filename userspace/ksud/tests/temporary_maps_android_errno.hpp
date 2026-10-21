#pragma once

// Only force-included by the synthetic Android-protocol fixture executable.
// Production and the ordinary Windows host tests keep their native errno ABI.
#include <cerrno>
#ifdef _WIN32
#undef EPERM
#undef ENOENT
#undef EINTR
#undef EIO
#undef EBADF
#undef EAGAIN
#undef ENOMEM
#undef EACCES
#undef EFAULT
#undef EBUSY
#undef EINVAL
#undef ENOTTY
#undef ENOSYS
#undef EPROTO
#undef EOVERFLOW
#undef EPROTONOSUPPORT
#undef EOPNOTSUPP
#undef ENOTSUP
#undef ESHUTDOWN
#undef ESTALE
#define EPERM 1
#define ENOENT 2
#define EINTR 4
#define EIO 5
#define EBADF 9
#define EAGAIN 11
#define ENOMEM 12
#define EACCES 13
#define EFAULT 14
#define EBUSY 16
#define EINVAL 22
#define ENOTTY 25
#define ENOSYS 38
#define EPROTO 71
#define EOVERFLOW 75
#define EPROTONOSUPPORT 93
#define EOPNOTSUPP 95
#define ENOTSUP 95
#define ESHUTDOWN 108
#define ESTALE 116
#endif

static_assert(EPERM == 1 && ENOENT == 2 && EINTR == 4 && EIO == 5 && EBADF == 9 && EAGAIN == 11 &&
                  ENOMEM == 12 && EACCES == 13 && EFAULT == 14 && EBUSY == 16 && EINVAL == 22 &&
                  ENOTTY == 25 && ENOSYS == 38 && EPROTO == 71 && EOVERFLOW == 75 &&
                  EPROTONOSUPPORT == 93 && EOPNOTSUPP == 95 && ENOTSUP == 95 && ESHUTDOWN == 108 &&
                  ESTALE == 116,
              "temporary Maps cross-language fixtures require the Android/Linux errno ABI");
