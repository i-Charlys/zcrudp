#include "protocol_profiles.h"

int main(void) {
  rudp_session_s session;
  rudp_scalar_tx_s tx = {0};
  uint8_t bytes[4];
  if (rudp_session_init(&session) != RUDP_OK) return 1;
  if (rudp_scalar_encode(&tx, 42, false, bytes, sizeof bytes) != 4) return 2;
  return bytes[3] == 42 ? 0 : 3;
}
