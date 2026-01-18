#include <czmq.h>
#include <stdint.h>
#include "zmq_interface.h"
#include "sdr.h"

static THREAD_RETURN THREAD_CALL acquire_thread(void *arg)
{

    zmq_config* zmq_info = (zmq_config*) arg;
        

    while(1)
    {
        //char buffer [1800000];
        unsigned char *buffer = malloc(1800000*sizeof(unsigned char));
        zmq_recv(zmq_info->requester, buffer, 1800000, 0);
        sdr_event_t ev = {
            .ev               = 1,
            .sample_rate      = 1800000,
            .center_frequency = 433000000,
            .buf              = buffer,
            .len              = 1800000,
         };
        printf("Running...\n");
        zmq_info->buf_len = 1800000;
        zmq_info->buf_num = buffer;
        zmq_info->process_ready = true;
        sleep(1);

    }

}

int zmq_start(zmq_config *zmq_info, uint32_t buf_num, uint32_t buf_len)
{

    zmq_info->context = zmq_ctx_new(); 
    zmq_info->requester = zmq_socket(zmq_info->context, ZMQ_SUB);
    zmq_connect (zmq_info->requester, "tcp://127.0.0.1:9001");
    int rc = zmq_setsockopt(zmq_info->requester, ZMQ_SUBSCRIBE, "", 0);
  
    if (rc < 0)
    { 
        return rc;
    }

/*
    zmq_info->buf_num = buf_num;
    zmq_info->buf_len = buf_len;
    zmq_info->thread = (pthread_t) zmq_info->thread;
#ifndef _WIN32
    // Block all signals from the worker thread
    sigset_t sigset;
    sigset_t oldset;
    sigfillset(&sigset);
    pthread_sigmask(SIG_SETMASK, &sigset, &oldset);
#endif
    int r = pthread_create(&zmq_info->thread, NULL, acquire_thread, zmq_info);
#ifndef _WIN32
    pthread_sigmask(SIG_SETMASK, &oldset, NULL);
#endif
    if (r) {
        fprintf(stderr, "%s: error in pthread_create, rc: %d\n", __func__, r);
    }
*/
    return rc;
}

