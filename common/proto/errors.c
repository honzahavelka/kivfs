#include "proto/errors.h"

const char *kivfs_strerror(int err)
{
    switch (err) {
    case KIVFS_OK:          return "success";

    /* protocol / packet */
    case KIVFS_EBADMAGIC:   return "bad magic number (foreign protocol or lost packet boundary)";
    case KIVFS_ETOOBIG:     return "payload exceeds KIVFS_MAX_PAYLOAD";
    case KIVFS_EMALFORMED:  return "malformed packet payload";
    case KIVFS_ENOMEM:      return "out of memory";
    case KIVFS_EEND:        return "no more records in packet";
    case KIVFS_ETYPE:       return "record has unexpected size for requested type";

    /* socket / channel */
    case KIVFS_ECLOSED:     return "connection closed by peer";
    case KIVFS_ETRUNC:      return "connection closed in the middle of a packet";
    case KIVFS_ETIMEOUT:    return "operation timed out";
    case KIVFS_EAGAIN:      return "operation would block";
    case KIVFS_EIO:         return "socket I/O error (see errno)";
    case KIVFS_ERESOLVE:    return "address resolution failed";
    case KIVFS_EUNEXPECTED: return "response does not match the request";
    case KIVFS_ECONNLOST:   return "connection lost before response (result unknown)";

    /* fronta / server */
    case KIVFS_ESHUTDOWN:   return "component is shutting down";
    case KIVFS_EINVAL:      return "invalid argument";
    case KIVFS_ENOTREADY:   return "dependency not ready";
    case KIVFS_ENOENT:      return "no such entry";
    case KIVFS_EPROTO:      return "protocol violation";

    /* konfigurace */
    case KIVFS_ECONFIG:     return "invalid configuration";
    }
    return "unknown error";
}
