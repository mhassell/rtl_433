//#include <zsock.h>

#ifndef _ZMQ_INTF
#define _ZMQ_INTF

#include <stdint.h>

#include "compat_pthread.h"

typedef struct z_cfg {
    char* address;    // tcp://127.0.0.1:9001
    char* tcp;        // tcp://127.0.0.1
    int   port;       // 9001
    void *context;    // zmq_ctx_new
    void *requester;  // zmq_socket

} zmq_config;

int zmq_start(zmq_config*);


#endif
