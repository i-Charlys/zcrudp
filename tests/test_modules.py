"""Verify real link boundaries, installed components, and optional RX storage."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CMAKE = os.environ.get('CMAKE', 'cmake')
CTEST = os.environ.get('CTEST', 'ctest')


def run(*args):
    subprocess.run(list(map(str, args)), check=True)


def configure(build, *options):
    run(CMAKE, '-S', ROOT, '-B', build, '-DCMAKE_BUILD_TYPE=Release',
        '-DZCRUDP_WARNINGS_AS_ERRORS=ON', *options)
    run(CMAKE, '--build', build, '--parallel', '2')
    run(CTEST, '--test-dir', build, '--output-on-failure')


CASES = {
    'codec': ('rudp_codec.h', '''rudp_record_s r = {.seq_num = 65535, .payload = {.value = 1234}}, decoded;
      uint8_t bytes[8];
      assert(rudp_pack_record(&r, bytes, sizeof(bytes)) == 8);
      assert(rudp_unpack_record(bytes, sizeof(bytes), &decoded) == RUDP_OK);
      assert(decoded.seq_num == 65535 && decoded.payload.value == 1234);'''),
    'reliability': ('rudp_reliability.h', '''rudp_context_s ctx;
      assert(rudp_init(&ctx) == RUDP_OK);
      assert(rudp_send(&ctx, (tfv_packet_u){.raw=0}, 0) == RUDP_OK);
      assert(rudp_recv_ack(&ctx, 1) == RUDP_OK); assert(ctx.head == ctx.tail);'''),
    'session': ('rudp_session.h', '''rudp_session_s session;
      assert(rudp_session_init(&session) == RUDP_OK);
      assert(rudp_session_send_reliable(&session, 0, (tfv_packet_u){.raw=0}, 0) == RUDP_OK);
      assert(rudp_session_reset(&session) == RUDP_OK);'''),
    'io': ('rudp_io.h', '''rudp_iovec_collection_s c = {.count = 0};
      uint8_t bytes[3], src[3] = {1,2,3};
      assert(rudp_iovec_add(&c, src, 3) == RUDP_OK);
      assert(rudp_iovec_flatten(&c, bytes, 3) == 3); assert(bytes[2] == 3);'''),
    'crypto': ('rudp_crypto.h', '''rudp_crypto_ctx_s tx, rx;
      uint8_t key[32] = {1}, wire[64], plain[32], msg[4] = {9, 8, 7, 6};
      assert(rudp_crypto_init(&tx, key, 42, 1) == RUDP_OK);
      assert(rudp_crypto_init(&rx, key, 42, 0) == RUDP_OK);
      assert(rudp_crypto_seal(&tx, NULL, 0, msg, 4, wire, sizeof(wire)) == 28);
      assert(rudp_crypto_open(&rx, NULL, 0, wire, 28, plain, sizeof(plain)) == 4);
      assert(plain[0] == 9 && plain[3] == 6);'''),
    'stream': ('rudp_stream.h', '''rudp_stream_tx_s tx = {.data = NULL};
      rudp_session_s session; uint8_t bytes[9] = {0};
      assert(rudp_session_init(&session) == RUDP_OK);
      assert(rudp_stream_start(&tx, bytes, sizeof(bytes)) == RUDP_OK);
      assert(rudp_stream_pump(&tx, &session, 0, 0) == 1);'''),
    'scalar': ('rudp_scalar.h', '''rudp_scalar_tx_s tx = {.next_seq = 0}; rudp_scalar_rx_s rx = {.last_seq = 0};
      rudp_scalar_sample_s samples[2]; uint8_t bytes[8];
      assert(rudp_scalar_encode(&tx, 100, true, bytes, 8) == 8);
      assert(rudp_scalar_encode(&tx, 200, true, bytes, 8) == 8);
      assert(rudp_scalar_decode(&rx, bytes, 8, true, samples, 2) == 2);
      assert(samples[0].value == 100 && samples[1].value == 200);'''),
    'motion': ('rudp_motion.h', '''rudp_motion_tx_s tx = {.scalar = {.next_seq = 0}}; uint8_t bytes[16];
      assert(rudp_motion_encode(&tx, 100, 1, 1, bytes, 16) == 1);'''),
    'nextgen': ('rudp_nextgen.h', '''static uint8_t mem[4 * RUDP_PAGE_SIZE];
      rudp_page_pool_s pool; rudp_page_extent_s ext;
      assert(rudp_page_pool_init(&pool, mem, 4) == RUDP_OK);
      assert(rudp_page_alloc(&pool, 64, &ext) == RUDP_OK);
      assert(rudp_page_resolve(&pool, ext) != NULL);'''),
}


def consumers(tmp, prefix, modules):
    source = tmp / 'consumer-source'
    source.mkdir()
    cmake = ['cmake_minimum_required(VERSION 3.21)', 'project(modules C)',
             'enable_testing()', 'find_package(zcrudp CONFIG REQUIRED COMPONENTS '
             + ' '.join(modules) + ')']
    for module in modules:
        header, body = CASES[module]
        (source / (module + '.c')).write_text(
            f'#include "{header}"\n#include <assert.h>\nint main(void) {{\n{body}\nreturn 0;\n}}\n')
        cmake.extend([f'add_executable({module} {module}.c)',
                      f'target_link_libraries({module} PRIVATE zcrudp::{module})',
                      f'add_test(NAME {module} COMMAND {module})'])
    (source / 'CMakeLists.txt').write_text('\n'.join(cmake))
    run(CMAKE, '-S', source, '-B', tmp / 'consumer-build', f'-DCMAKE_PREFIX_PATH={prefix}')
    run(CMAKE, '--build', tmp / 'consumer-build', '--parallel', '2')
    run(CTEST, '--test-dir', tmp / 'consumer-build', '--output-on-failure')


with tempfile.TemporaryDirectory(prefix='zcrudp-modules-') as temporary:
    tmp = Path(temporary)
    for shared in ('OFF', 'ON'):
        case = tmp / ('full-' + shared)
        configure(case / 'build', '-DBUILD_SHARED_LIBS=' + shared)
        run(CMAKE, '--install', case / 'build', '--prefix', case / 'prefix')
        consumers(case, case / 'prefix', list(CASES))
    minimal = tmp / 'minimal'
    configure(minimal / 'build', '-DZCRUDP_BUILD_RELIABILITY=OFF',
              '-DZCRUDP_BUILD_SESSION=OFF', '-DZCRUDP_BUILD_IO=OFF',
              '-DZCRUDP_BUILD_STREAM=OFF', '-DZCRUDP_BUILD_MOTION=OFF',
              '-DZCRUDP_BUILD_NEXTGEN=OFF')
    run(CMAKE, '--install', minimal / 'build', '--prefix', minimal / 'prefix')
    consumers(minimal, minimal / 'prefix', ['codec', 'scalar'])
    installed = {p.name for p in (minimal / 'prefix').rglob('libzcrudp*')}
    assert installed == {'libzcrudp_codec.a', 'libzcrudp_scalar.a'}, installed
    external = tmp / 'external'
    configure(external, '-DZCRUDP_EXTERNAL_RX_STORAGE=ON')
    # ABI propagation must also work through an installed package.
    run(CMAKE, '--install', external, '--prefix', tmp / 'external-prefix')
    external_consumer = tmp / 'external-consumer'
    external_consumer.mkdir()
    consumers(external_consumer, tmp / 'external-prefix', ['session'])
print('PASS: independent installed module consumers, static/shared, minimal build, external RX ABI')
