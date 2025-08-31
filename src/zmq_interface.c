#include <czmq.h>

#include "zmq_interface.h"

zsock_t *z_connect(zmq_config *info)
{
    zsock_t *sink = zsock_new_sub(info->address, "");
    return sink;
}

void z_disconnect(zmq_config *info)
{
   (void) zsock_new_sub(info->address, 0);    
    
}
