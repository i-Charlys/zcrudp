// Compile-only check: every public header must be consumable from C++
// (g++ and clang++), as promised by the extern "C" guards.
#include "protocol_tfv.h"
#include "rudp_config.h"
#include "rudp_types.h"
#include "rudp_codec.h"
#include "rudp_reliability.h"
#include "rudp_session.h"
#include "rudp_io.h"
#include "rudp_crypto.h"
#include "rudp_stream.h"
#include "rudp_scalar.h"
#include "rudp_motion.h"
#include "rudp_nextgen.h"
#include "protocol_rudp.h"
#include "protocol_profiles.h"
#include "../bindings/cpp/zcrudp.hpp"

int main() {
  rudp_session_s session;
  tfv_packet_u payload{};
  payload.type = 1;
  if (rudp_session_init(&session) != RUDP_OK) return 1;
  zcrudp::Session cpp_session;
  rudp_pacer_s pacer;
  if (rudp_pacer_init(&pacer, 10000U, 2000U, 50000U, 5000U, 0ULL) != RUDP_OK) return 1;
  if (cpp_session.queue_unreliable(0, payload) != RUDP_OK) return 1;
  return rudp_session_send_reliable(&session, 0, payload, 0) == RUDP_OK ? 0 : 1;
}
