#include <czmq.h>
#include <zsock.h>

#include "rtl_433.h"

zsock_t* z_connect(zmq_config *info);

void z_disconnect(zmq_config *info);
