#ifndef RC_IPC_H
#define RC_IPC_H
#include "riden_core.h"
#include <stdatomic.h>
int rc_ipc_run(rc_core *, unsigned port, atomic_int *stop, int writes, int multiwrite);
#endif
