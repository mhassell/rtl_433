//#include <zsock.h>

#ifndef _ZMQ_INTF
#define _ZMQ_INTF

#include "compat_pthread.h"
#include <stdint.h>

typedef struct z_cfg {
    char* address;    // tcp://127.0.0.1:9001
    char* tcp;        // tcp://127.0.0.1
    int   port;       // 9001
    void *context;    // zmq_ctx_new
    void *requester;  // zmq_socket
    uint32_t buf_num;
    uint32_t buf_len;
    pthread_t thread;
    pthread_mutex_t lock; ///< lock for exit_acquire
    int exit_acquire;

} zmq_config;

int zmq_start(zmq_config*, uint32_t, uint32_t);


#endif
