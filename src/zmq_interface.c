#include <czmq.h>
#include <stdint.h>
#include "zmq_interface.h"
#include "sdr.h"

int zmq_start(zmq_config *zmq_info, uint32_t buf_num, uint32_t buf_len)
{

    zmq_info->context = zmq_ctx_new(); 
    zmq_info->requester = zmq_socket(zmq_info->context, ZMQ_SUB);
    zmq_connect (zmq_info->requester, "tcp://127.0.0.1:9001");
    int rc = zmq_setsockopt(zmq_info->requester, ZMQ_SUBSCRIBE, "", 0);
  
    return rc;
}

