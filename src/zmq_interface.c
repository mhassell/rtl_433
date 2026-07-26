#include <czmq.h>
#include <stdint.h>
#include "zmq_interface.h"
#include "sdr.h"

int zmq_start(zmq_config *zmq_info)
{

    zmq_info->context = zmq_ctx_new(); 
    zmq_info->requester = zmq_socket(zmq_info->context, ZMQ_SUB);
    const char *addr = (zmq_info->address && *zmq_info->address)
                       ? zmq_info->address
                       : "ipc:///tmp/gqrx_iq.sock";
    zmq_connect(zmq_info->requester, addr);
    int rc = zmq_setsockopt(zmq_info->requester, ZMQ_SUBSCRIBE, "", 0);
  
    return rc;
}

