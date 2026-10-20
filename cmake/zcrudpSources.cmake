# Explicit module source lists, also used for alternate-ABI test builds.
set(ZCRUDP_CODEC_SOURCES src/codec.c)
set(ZCRUDP_RELIABILITY_SOURCES src/reliability.c)
set(ZCRUDP_SESSION_SOURCES src/session.c src/session_tx.c src/session_rx.c src/rx.c)
set(ZCRUDP_IO_SOURCES src/io.c)
set(ZCRUDP_CORE_SOURCES ${ZCRUDP_CODEC_SOURCES} ${ZCRUDP_RELIABILITY_SOURCES}
  ${ZCRUDP_SESSION_SOURCES} ${ZCRUDP_IO_SOURCES})
