// Compile-only check: every public header must be consumable from C++
// (g++ and clang++), as promised by the extern "C" guards.
#include "protocol_tfv.h"
#include "rudp_config.h"
#include "rudp_types.h"
#include "rudp_codec.h"
#include "rudp_reliability.h"
#include "rudp_session.h"
#include "rudp_io.h"
#include "rudp_stream.h"
#include "rudp_scalar.h"
#include "rudp_motion.h"
#include "protocol_rudp.h"
#include "protocol_profiles.h"

int main() {
  rudp_session_s session;
  tfv_packet_u payload{};
  payload.type = 1;
  if (rudp_session_init(&session) != RUDP_OK) return 1;
  return rudp_session_send_reliable(&session, 0, payload, 0) == RUDP_OK ? 0 : 1;
}
