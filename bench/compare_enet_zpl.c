#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>

#include "compare_enet_zpl.h"

#define MTU 1400

static int zpl_next_socket = 0;
static int zpl_connected = 0;
static uint32_t *g_now = NULL;
static uint32_t *g_loss_percent = NULL;
static int *g_failed = NULL;

static int mock_socket(int domain, int type, int protocol) {
  (void)domain; (void)type; (void)protocol;
  assert(zpl_next_socket < 2);
  return zpl_next_socket++;
}

static int mock_bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
  (void)sockfd; (void)addr; (void)addrlen;
  return 0;
}

static int mock_getsockname(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
  if (addr && addrlen && *addrlen >= sizeof(struct sockaddr_in)) {
    struct sockaddr_in *sin = (struct sockaddr_in *)addr;
    sin->sin_family = AF_INET;
    sin->sin_port = htons((uint16_t)(10000 + sockfd));
    sin->sin_addr.s_addr = htonl(0x7f000001u + (unsigned)sockfd);
    *addrlen = sizeof(struct sockaddr_in);
  }
  return 0;
}

static int mock_setsockopt(int s, int level, int optname, const void *optval, socklen_t optlen) {
  (void)s; (void)level; (void)optname; (void)optval; (void)optlen;
  return 0;
}

static int mock_fcntl(int fd, int cmd, ...) {
  (void)fd; (void)cmd;
  return 0;
}

static int mock_close(int fd) {
  (void)fd;
  return 0;
}

static int mock_poll(struct pollfd *fds, nfds_t nfds, int timeout) {
  (void)fds; (void)nfds; (void)timeout;
  return 0;
}

static ssize_t mock_sendmsg(int sockfd, const struct msghdr *msg, int flags) {
  (void)flags;
  uint8_t bytes[MTU];
  unsigned len = 0;
  for (size_t i = 0; i < msg->msg_iovlen; i++) {
    assert(len + msg->msg_iov[i].iov_len <= MTU);
    memcpy(bytes + len, msg->msg_iov[i].iov_base, msg->msg_iov[i].iov_len);
    len += (unsigned)msg->msg_iov[i].iov_len;
  }
  link_send(sockfd, bytes, len);
  return (ssize_t)len;
}

static ssize_t mock_recvmsg(int sockfd, struct msghdr *msg, int flags) {
  (void)flags;
  assert(msg->msg_iovlen >= 1 && msg->msg_iov[0].iov_len >= MTU);
  unsigned len = link_receive(sockfd, msg->msg_iov[0].iov_base);
  if (!len) {
    errno = EWOULDBLOCK;
    return -1;
  }
  if (msg->msg_name) {
    struct sockaddr_in *sin = (struct sockaddr_in *)msg->msg_name;
    sin->sin_family = AF_INET;
    sin->sin_port = htons((uint16_t)(10000 + (1 - sockfd)));
    sin->sin_addr.s_addr = htonl(0x7f000001u + (unsigned)(1 - sockfd));
    msg->msg_namelen = sizeof(struct sockaddr_in);
  }
  return (ssize_t)len;
}

static int mock_clock_gettime(clockid_t clk_id, struct timespec *tp) {
  (void)clk_id;
  uint32_t current_now = g_now ? *g_now : 1000;
  tp->tv_sec = current_now / 1000;
  tp->tv_nsec = (current_now % 1000) * 1000000;
  return 0;
}

#define socket(...) mock_socket(__VA_ARGS__)
#define bind(...) mock_bind(__VA_ARGS__)
#define getsockname(...) mock_getsockname(__VA_ARGS__)
#define setsockopt(...) mock_setsockopt(__VA_ARGS__)
#define fcntl(...) mock_fcntl(__VA_ARGS__)
#define close(...) mock_close(__VA_ARGS__)
#define poll(...) mock_poll(__VA_ARGS__)
#define sendmsg(...) mock_sendmsg(__VA_ARGS__)
#define recvmsg(...) mock_recvmsg(__VA_ARGS__)
#define clock_gettime(...) mock_clock_gettime(__VA_ARGS__)

#define ENET_IPV4_ONLY 1

/* Prefix ALL zpl-c symbols to avoid collision with lsalzman/enet */
#define callbacks zpl_callbacks
#define enet_address_get_host_ip_new zpl_enet_address_get_host_ip_new
#define enet_address_get_host_ip_old zpl_enet_address_get_host_ip_old
#define enet_address_get_host_new zpl_enet_address_get_host_new
#define enet_address_get_host_old zpl_enet_address_get_host_old
#define enet_address_set_host_ip_new zpl_enet_address_set_host_ip_new
#define enet_address_set_host_ip_old zpl_enet_address_set_host_ip_old
#define enet_address_set_host_new zpl_enet_address_set_host_new
#define enet_address_set_host_old zpl_enet_address_set_host_old
#define enet_crc32 zpl_enet_crc32
#define enet_deinitialize zpl_enet_deinitialize
#define enet_free zpl_enet_free
#define enet_host_bandwidth_limit zpl_enet_host_bandwidth_limit
#define enet_host_bandwidth_throttle zpl_enet_host_bandwidth_throttle
#define enet_host_broadcast zpl_enet_host_broadcast
#define enet_host_channel_limit zpl_enet_host_channel_limit
#define enet_host_check_events zpl_enet_host_check_events
#define enet_host_compress zpl_enet_host_compress
#define enet_host_connect zpl_enet_host_connect
#define enet_host_create zpl_enet_host_create
#define enet_host_destroy zpl_enet_host_destroy
#define enet_host_flush zpl_enet_host_flush
#define enet_host_get_bytes_received zpl_enet_host_get_bytes_received
#define enet_host_get_bytes_sent zpl_enet_host_get_bytes_sent
#define enet_host_get_mtu zpl_enet_host_get_mtu
#define enet_host_get_packets_received zpl_enet_host_get_packets_received
#define enet_host_get_packets_sent zpl_enet_host_get_packets_sent
#define enet_host_get_peers_count zpl_enet_host_get_peers_count
#define enet_host_get_received_data zpl_enet_host_get_received_data
#define enet_host_random zpl_enet_host_random
#define enet_host_random_seed zpl_enet_host_random_seed
#define enet_host_send_raw zpl_enet_host_send_raw
#define enet_host_send_raw_ex zpl_enet_host_send_raw_ex
#define enet_host_service zpl_enet_host_service
#define enet_host_set_intercept zpl_enet_host_set_intercept
#define enet_in6addr_lookup_host zpl_enet_in6addr_lookup_host
#define enet_inaddr_lookup_host zpl_enet_inaddr_lookup_host
#define enet_inaddr_map4to6 zpl_enet_inaddr_map4to6
#define enet_inaddr_map6to4 zpl_enet_inaddr_map6to4
#define enet_initialize zpl_enet_initialize
#define enet_initialize_with_callbacks zpl_enet_initialize_with_callbacks
#define enet_linked_version zpl_enet_linked_version
#define enet_list_clear zpl_enet_list_clear
#define enet_list_insert zpl_enet_list_insert
#define enet_list_move zpl_enet_list_move
#define enet_list_remove zpl_enet_list_remove
#define enet_list_size zpl_enet_list_size
#define enet_malloc zpl_enet_malloc
#define enet_packet_copy zpl_enet_packet_copy
#define enet_packet_create zpl_enet_packet_create
#define enet_packet_create_offset zpl_enet_packet_create_offset
#define enet_packet_destroy zpl_enet_packet_destroy
#define enet_packet_get_data zpl_enet_packet_get_data
#define enet_packet_get_length zpl_enet_packet_get_length
#define enet_packet_resize zpl_enet_packet_resize
#define enet_packet_set_free_callback zpl_enet_packet_set_free_callback
#define enet_peer_disconnect zpl_enet_peer_disconnect
#define enet_peer_disconnect_later zpl_enet_peer_disconnect_later
#define enet_peer_disconnect_now zpl_enet_peer_disconnect_now
#define enet_peer_dispatch_incoming_reliable_commands zpl_enet_peer_dispatch_incoming_reliable_commands
#define enet_peer_dispatch_incoming_unreliable_commands zpl_enet_peer_dispatch_incoming_unreliable_commands
#define enet_peer_get_bytes_received zpl_enet_peer_get_bytes_received
#define enet_peer_get_bytes_sent zpl_enet_peer_get_bytes_sent
#define enet_peer_get_data zpl_enet_peer_get_data
#define enet_peer_get_id zpl_enet_peer_get_id
#define enet_peer_get_ip zpl_enet_peer_get_ip
#define enet_peer_get_packets_lost zpl_enet_peer_get_packets_lost
#define enet_peer_get_packets_sent zpl_enet_peer_get_packets_sent
#define enet_peer_get_port zpl_enet_peer_get_port
#define enet_peer_get_rtt zpl_enet_peer_get_rtt
#define enet_peer_get_state zpl_enet_peer_get_state
#define enet_peer_has_outgoing_commands zpl_enet_peer_has_outgoing_commands
#define enet_peer_on_connect zpl_enet_peer_on_connect
#define enet_peer_on_disconnect zpl_enet_peer_on_disconnect
#define enet_peer_ping zpl_enet_peer_ping
#define enet_peer_ping_interval zpl_enet_peer_ping_interval
#define enet_peer_queue_acknowledgement zpl_enet_peer_queue_acknowledgement
#define enet_peer_queue_incoming_command zpl_enet_peer_queue_incoming_command
#define enet_peer_queue_outgoing_command zpl_enet_peer_queue_outgoing_command
#define enet_peer_receive zpl_enet_peer_receive
#define enet_peer_reset zpl_enet_peer_reset
#define enet_peer_reset_queues zpl_enet_peer_reset_queues
#define enet_peer_send zpl_enet_peer_send
#define enet_peer_set_data zpl_enet_peer_set_data
#define enet_peer_setup_outgoing_command zpl_enet_peer_setup_outgoing_command
#define enet_peer_throttle zpl_enet_peer_throttle
#define enet_peer_throttle_configure zpl_enet_peer_throttle_configure
#define enet_peer_timeout zpl_enet_peer_timeout
#define enet_protocol_command_size zpl_enet_protocol_command_size
#define enet_socket_accept zpl_enet_socket_accept
#define enet_socket_bind zpl_enet_socket_bind
#define enet_socket_connect zpl_enet_socket_connect
#define enet_socket_create zpl_enet_socket_create
#define enet_socket_destroy zpl_enet_socket_destroy
#define enet_socket_get_address zpl_enet_socket_get_address
#define enet_socket_get_option zpl_enet_socket_get_option
#define enet_socket_listen zpl_enet_socket_listen
#define enet_socket_receive zpl_enet_socket_receive
#define enet_socket_send zpl_enet_socket_send
#define enet_socket_set_option zpl_enet_socket_set_option
#define enet_socket_shutdown zpl_enet_socket_shutdown
#define enet_socket_wait zpl_enet_socket_wait
#define enet_socketset_select zpl_enet_socketset_select
#define enet_time_get zpl_enet_time_get

#define ENET_IMPLEMENTATION
#include "enet.h"

static ENetHost *hosts[2];
static ENetPeer *remote;

void zpl_enet_service(int side, int setup) {
  ENetEvent e;
  int ret;
  while ((ret = zpl_enet_host_service(hosts[side], &e, 0)) > 0) {
    if (e.type == ENET_EVENT_TYPE_CONNECT) {
      zpl_connected++;
    } else if (e.type == ENET_EVENT_TYPE_RECEIVE) {
      assert(!setup && side == 1);
      delivery(e.packet->data, e.packet->dataLength);
      zpl_enet_packet_destroy(e.packet);
    } else if (e.type == ENET_EVENT_TYPE_DISCONNECT || e.type == ENET_EVENT_TYPE_DISCONNECT_TIMEOUT) {
      if (g_failed) *g_failed = 1;
    }
  }
  assert(ret == 0);
}

void zpl_enet_init_engine(uint32_t *now_ptr, uint32_t *loss_percent_ptr, int *failed_ptr) {
  g_now = now_ptr;
  g_loss_percent = loss_percent_ptr;
  g_failed = failed_ptr;
  zpl_next_socket = zpl_connected = 0;

  assert(zpl_enet_initialize() == 0);
  for (int side = 0; side < 2; side++) {
    ENetAddress a;
    a.host.s_addr = htonl(0x7f000001u + (unsigned)side);
    a.port = (enet_uint16)(10000 + side);
    a.sin6_scope_id = 0;
    hosts[side] = zpl_enet_host_create(&a, 1, 1, 0, 0);
    assert(hosts[side]);
    hosts[side]->mtu = MTU;
    hosts[side]->randomSeed = 42;
  }

  ENetAddress target;
  target.host.s_addr = htonl(0x7f000001u + 1);
  target.port = 10001;
  target.sin6_scope_id = 0;
  remote = zpl_enet_host_connect(hosts[0], &target, 1, 0);
  assert(remote);

  /* Exclude connection establishment from loss measurements */
  uint32_t saved_loss = *g_loss_percent;
  *g_loss_percent = 0;
  for (; *g_now < 2000 && (zpl_connected < 2 || link_has_pending()); (*g_now)++) {
    zpl_enet_service(0, 1);
    zpl_enet_service(1, 1);
  }
  assert(zpl_connected == 2 && !link_has_pending());
  *g_loss_percent = saved_loss;
}

unsigned zpl_enet_pending(void) {
  return (unsigned)(zpl_enet_list_size(&remote->sentReliableCommands) +
                    zpl_enet_list_size(&remote->outgoingCommands) +
                    zpl_enet_list_size(&remote->outgoingSendReliableCommands));
}

void zpl_enet_submit(uint16_t admitted) {
  uint8_t bytes[4] = {1, 0, (uint8_t)(admitted >> 8), (uint8_t)admitted};
  ENetPacket *packet = zpl_enet_packet_create(bytes, 4, ENET_PACKET_FLAG_RELIABLE);
  assert(packet);
  assert(zpl_enet_peer_send(remote, 0, packet) == 0);
}

void zpl_enet_cleanup(void) {
  for (int side = 0; side < 2; side++) {
    if (hosts[side]) {
      zpl_enet_host_destroy(hosts[side]);
      hosts[side] = NULL;
    }
  }
  zpl_enet_deinitialize();
}
