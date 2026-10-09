// Host-only stand-in for link.c's state query, so screens can show any link
// state without running a handshake.
#include "link.h"

mesh_link_state_t fake_link_state = MESH_LINK_READY;

mesh_link_state_t mesh_link_state(void) { return fake_link_state; }
