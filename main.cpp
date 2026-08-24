/*
 * PS5 Date/Time Sync by Deckerr97
 *
 * One-shot payload for consoles whose RTC/CMOS backup battery no longer
 * retains the clock across a full power-off.  It obtains UTC from SNTP,
 * corrects for half of the measured round-trip time, sets the system clock,
 * and exits after reporting the result through the notification center.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

extern "C" {
int sceNetInit();
int sceNetPoolCreate(const char*, int, int);
int sceNetPoolDestroy(int);

struct NotifyRequest {
  char useless[45];
  char message[3075];
};

int sceKernelSendNotificationRequest(int device, NotifyRequest* request,
                                     size_t size, int unused);

// Keep the payload independent of feature-test macros in the SDK's FreeBSD
// headers.  Both symbols are exported by libkernel on the target.
int gettimeofday(struct timeval*, struct timezone*);
int settimeofday(const struct timeval*, const struct timezone*);
}

namespace {

constexpr uint16_t kNtpPort = 123;
constexpr size_t kNtpPacketSize = 48;
constexpr size_t kMaxNtpResponseSize = 512;
constexpr size_t kNtpOriginateTimestampOffset = 24;
constexpr size_t kNtpTransmitTimestampOffset = 40;
constexpr size_t kNtpTimestampSize = 8;
constexpr uint8_t kNtpVersion = 4;
constexpr uint8_t kNtpClientMode = 3;
constexpr uint8_t kNtpServerMode = 4;
constexpr uint8_t kNtpClientFlags = (kNtpVersion << 3) | kNtpClientMode;
constexpr int kNetworkPoolSize = 64 * 1024;
constexpr int kSocketTimeoutSeconds = 3;
constexpr int kAttemptsPerServer = 2;
constexpr unsigned int kRetryDelaySeconds = 1;
constexpr int64_t kMaxClockVerificationDeltaMicroseconds = 2 * 1000000LL;
constexpr const char* kNotificationText =
    "PS5 time & date sync by Deckerr97";

// NTP timestamps count from 1900-01-01; Unix timestamps count from
// 1970-01-01.
constexpr uint32_t kNtpToUnixEpoch = 2208988800U;
constexpr int64_t kMinimumUnixTime = 946684800LL;       // 2000-01-01
constexpr int64_t kMaximumUnixTime = 4102444800LL;      // 2100-01-01

const char* const kNtpServers[] = {
    // Cloudflare publishes these anycast addresses for clients that cannot
    // resolve time.cloudflare.com. Keep them first so clock recovery does
    // not depend on working DNS.
    "162.159.200.1",
    "162.159.200.123",
    "time.cloudflare.com",
    "pool.ntp.org",
    "time.google.com",
};

void log_message(const char* format, ...) {
  char line[768] = {};
  va_list arguments;
  va_start(arguments, format);
  int written = vsnprintf(line, sizeof(line), format, arguments);
  va_end(arguments);
  if (written <= 0) {
    return;
  }

  size_t length = static_cast<size_t>(written);
  if (length >= sizeof(line)) {
    length = sizeof(line) - 1;
  }
  fwrite(line, 1, length, stdout);
  fflush(stdout);
}

void notify_user() {
  NotifyRequest request = {};
  snprintf(request.message, sizeof(request.message), "%s", kNotificationText);
  int result = sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
  log_message("[notification] rc=%d: %s\n", result, request.message);
}

class NetworkSession {
 public:
  NetworkSession() = default;
  NetworkSession(const NetworkSession&) = delete;
  NetworkSession& operator=(const NetworkSession&) = delete;

  ~NetworkSession() {
    if (pool_id_ >= 0) {
      int result = sceNetPoolDestroy(pool_id_);
      log_message("sceNetPoolDestroy rc=%d\n", result);
    }
  }

  bool initialize() {
    int result = sceNetInit();
    if (result != 0) {
      log_message("sceNetInit failed: rc=%d\n", result);
      return false;
    }

    pool_id_ = sceNetPoolCreate("ps5-date-time-sync", kNetworkPoolSize, 0);
    if (pool_id_ < 0) {
      log_message("sceNetPoolCreate failed: rc=%d\n", pool_id_);
      return false;
    }
    return true;
  }

 private:
  int pool_id_ = -1;
};

bool monotonic_microseconds(int64_t* output) {
  if (!output) {
    return false;
  }

  struct timespec value = {};
  if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
    return false;
  }

  *output = static_cast<int64_t>(value.tv_sec) * 1000000LL +
            static_cast<int64_t>(value.tv_nsec / 1000);
  return true;
}

uint32_t read_big_endian_u32(const uint8_t* bytes) {
  return (static_cast<uint32_t>(bytes[0]) << 24) |
         (static_cast<uint32_t>(bytes[1]) << 16) |
         (static_cast<uint32_t>(bytes[2]) << 8) |
         static_cast<uint32_t>(bytes[3]);
}

void write_big_endian_u32(uint8_t* bytes, uint32_t value) {
  bytes[0] = static_cast<uint8_t>(value >> 24);
  bytes[1] = static_cast<uint8_t>(value >> 16);
  bytes[2] = static_cast<uint8_t>(value >> 8);
  bytes[3] = static_cast<uint8_t>(value);
}

bool write_client_transmit_timestamp(uint8_t* packet) {
  if (!packet) {
    return false;
  }

  struct timeval current = {};
  if (gettimeofday(&current, nullptr) != 0) {
    log_message("gettimeofday for NTP request failed: errno=%d\n", errno);
    return false;
  }
  if (current.tv_usec < 0 || current.tv_usec >= 1000000) {
    log_message("gettimeofday returned invalid microseconds: %lld\n",
                static_cast<long long>(current.tv_usec));
    return false;
  }

  int64_t unix_seconds = static_cast<int64_t>(current.tv_sec);
  if (unix_seconds < -static_cast<int64_t>(kNtpToUnixEpoch)) {
    log_message("system clock is earlier than the NTP epoch\n");
    return false;
  }

  uint64_t full_ntp_seconds = static_cast<uint64_t>(
      unix_seconds + static_cast<int64_t>(kNtpToUnixEpoch));
  uint32_t wire_seconds = static_cast<uint32_t>(full_ntp_seconds);
  uint32_t wire_fraction = static_cast<uint32_t>(
      (static_cast<uint64_t>(current.tv_usec) << 32) / 1000000ULL);

  // The server copies this value into its Originate Timestamp. Keep it
  // nonzero so the response can be tied to this exact request.
  if (wire_seconds == 0 && wire_fraction == 0) {
    wire_fraction = 1;
  }
  write_big_endian_u32(packet + kNtpTransmitTimestampOffset, wire_seconds);
  write_big_endian_u32(packet + kNtpTransmitTimestampOffset + 4,
                       wire_fraction);
  return true;
}

bool configure_socket_timeouts(int socket_fd) {
  struct timeval timeout = {};
  timeout.tv_sec = kSocketTimeoutSeconds;
  if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                 sizeof(timeout)) != 0) {
    log_message("SO_RCVTIMEO could not be set: errno=%d\n", errno);
    return false;
  }
  if (setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                 sizeof(timeout)) != 0) {
    log_message("SO_SNDTIMEO could not be set: errno=%d\n", errno);
    return false;
  }
  return true;
}

bool resolve_server(const char* hostname, struct sockaddr_in* address) {
  if (!hostname || !address) {
    return false;
  }

  *address = {};
  int numeric_result = inet_pton(AF_INET, hostname, &address->sin_addr);
  if (numeric_result == 1) {
    address->sin_family = AF_INET;
    address->sin_port = htons(kNtpPort);
    return true;
  }
  if (numeric_result < 0) {
    log_message("IPv4 address parsing failed for %s: errno=%d\n", hostname,
                errno);
    return false;
  }

  struct addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_protocol = IPPROTO_UDP;

  struct addrinfo* results = nullptr;
  int result = getaddrinfo(hostname, nullptr, &hints, &results);
  if (result != 0) {
    log_message("DNS lookup failed for %s: %s\n", hostname,
                gai_strerror(result));
    return false;
  }

  bool resolved = false;
  for (struct addrinfo* current = results; current; current = current->ai_next) {
    if (!current->ai_addr || current->ai_addrlen < sizeof(struct sockaddr_in)) {
      continue;
    }

    memcpy(address, current->ai_addr, sizeof(struct sockaddr_in));
    address->sin_port = htons(kNtpPort);
    resolved = true;
    break;
  }

  freeaddrinfo(results);
  if (!resolved) {
    log_message("DNS lookup for %s returned no IPv4 address\n", hostname);
  }
  return resolved;
}

bool request_time(const struct sockaddr_in& server, struct timeval* output) {
  if (!output) {
    return false;
  }

  int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (socket_fd < 0) {
    log_message("socket failed: errno=%d\n", errno);
    return false;
  }

  if (!configure_socket_timeouts(socket_fd)) {
    close(socket_fd);
    return false;
  }

  uint8_t request[kNtpPacketSize] = {};
  // Leap indicator 0, version 4, client mode 3.
  request[0] = kNtpClientFlags;
  if (!write_client_transmit_timestamp(request)) {
    close(socket_fd);
    return false;
  }

  int64_t start_microseconds = 0;
  int64_t end_microseconds = 0;
  bool have_start = monotonic_microseconds(&start_microseconds);

  ssize_t sent = sendto(socket_fd, request, sizeof(request), 0,
                         reinterpret_cast<const struct sockaddr*>(&server),
                         sizeof(server));
  if (sent != static_cast<ssize_t>(sizeof(request))) {
    log_message("sendto failed: errno=%d\n", errno);
    close(socket_fd);
    return false;
  }

  uint8_t response[kMaxNtpResponseSize] = {};
  struct sockaddr_in source = {};
  socklen_t source_length = sizeof(source);
  ssize_t received = recvfrom(
      socket_fd, response, sizeof(response), 0,
      reinterpret_cast<struct sockaddr*>(&source), &source_length);
  int receive_errno = errno;
  bool have_end = monotonic_microseconds(&end_microseconds);
  close(socket_fd);

  if (received < 0) {
    log_message("recvfrom failed: errno=%d\n", receive_errno);
    return false;
  }
  if (received < static_cast<ssize_t>(kNtpPacketSize)) {
    log_message("short NTP response: %lld bytes\n",
                static_cast<long long>(received));
    return false;
  }
  if (source_length < sizeof(source) || source.sin_family != AF_INET ||
      source.sin_port != server.sin_port ||
      source.sin_addr.s_addr != server.sin_addr.s_addr) {
    log_message("NTP response came from an unexpected endpoint\n");
    return false;
  }
  if (memcmp(response + kNtpOriginateTimestampOffset,
             request + kNtpTransmitTimestampOffset,
             kNtpTimestampSize) != 0) {
    log_message("NTP response does not match the request timestamp\n");
    return false;
  }

  uint8_t flags = response[0];
  uint8_t leap_indicator = flags >> 6;
  uint8_t version = (flags >> 3) & 0x07;
  uint8_t mode = flags & 0x07;
  uint8_t stratum = response[1];
  if (leap_indicator == 3 || mode != kNtpServerMode ||
      version != kNtpVersion ||
      stratum == 0 || stratum >= 16) {
    log_message("invalid NTP response: LI=%u version=%u mode=%u stratum=%u\n",
                static_cast<unsigned int>(leap_indicator),
                static_cast<unsigned int>(version), static_cast<unsigned int>(mode),
                static_cast<unsigned int>(stratum));
    return false;
  }

  uint32_t ntp_seconds = read_big_endian_u32(response + 40);
  uint32_t ntp_fraction = read_big_endian_u32(response + 44);
  if (ntp_seconds == 0 && ntp_fraction == 0) {
    log_message("NTP response has an empty transmit timestamp\n");
    return false;
  }

  // NTP's seconds field wrapped in 2036.  A value below the Unix-epoch
  // offset is therefore treated as the beginning of the next NTP era.
  uint64_t full_ntp_seconds = ntp_seconds;
  if (full_ntp_seconds < kNtpToUnixEpoch) {
    full_ntp_seconds += (1ULL << 32);
  }
  int64_t unix_seconds = static_cast<int64_t>(
      full_ntp_seconds - static_cast<uint64_t>(kNtpToUnixEpoch));
  if (unix_seconds < kMinimumUnixTime || unix_seconds > kMaximumUnixTime) {
    log_message("NTP response is outside the accepted date range: %lld\n",
                static_cast<long long>(unix_seconds));
    return false;
  }

  int64_t microseconds =
      static_cast<int64_t>((static_cast<uint64_t>(ntp_fraction) * 1000000ULL) >>
                           32);
  if (have_start && have_end && end_microseconds > start_microseconds) {
    microseconds += (end_microseconds - start_microseconds) / 2;
  }

  int64_t total_microseconds = unix_seconds * 1000000LL + microseconds;
  output->tv_sec = static_cast<time_t>(total_microseconds / 1000000LL);
  output->tv_usec = static_cast<suseconds_t>(total_microseconds % 1000000LL);
  return true;
}

bool format_utc(const struct timeval& value, char* output, size_t output_size) {
  if (!output || output_size == 0) {
    return false;
  }

  time_t seconds = value.tv_sec;
  struct tm broken_down = {};
  if (!gmtime_r(&seconds, &broken_down)) {
    return false;
  }

  return strftime(output, output_size, "%Y-%m-%d %H:%M:%S UTC", &broken_down) >
         0;
}

bool set_system_time(const struct timeval& value) {
  struct timeval candidate = value;
  if (settimeofday(&candidate, nullptr) != 0) {
    log_message("settimeofday failed: errno=%d\n", errno);
    return false;
  }

  struct timeval verified = {};
  if (gettimeofday(&verified, nullptr) != 0) {
    log_message("gettimeofday verification failed: errno=%d\n", errno);
    return false;
  }

  int64_t difference =
      (static_cast<int64_t>(verified.tv_sec) -
       static_cast<int64_t>(candidate.tv_sec)) *
          1000000LL +
      static_cast<int64_t>(verified.tv_usec) -
      static_cast<int64_t>(candidate.tv_usec);
  if (difference < 0) {
    difference = -difference;
  }
  log_message("clock verification delta: %lld microseconds\n",
              static_cast<long long>(difference));
  if (difference > kMaxClockVerificationDeltaMicroseconds) {
    log_message("clock verification exceeded the allowed delta\n");
    return false;
  }
  return true;
}

int synchronize_system_time() {
  NetworkSession network;
  if (!network.initialize()) {
    return 1;
  }

  struct timeval network_time = {};
  bool received_time = false;
  for (const char* server_name : kNtpServers) {
    struct sockaddr_in server = {};
    if (!resolve_server(server_name, &server)) {
      continue;
    }

    for (int attempt = 0; attempt < kAttemptsPerServer; ++attempt) {
      log_message("querying %s (attempt %d/%d)\n", server_name, attempt + 1,
                  kAttemptsPerServer);
      if (request_time(server, &network_time)) {
        received_time = true;
        break;
      }
      if (attempt + 1 < kAttemptsPerServer) {
        sleep(kRetryDelaySeconds);
      }
    }
    if (received_time) {
      break;
    }
  }

  if (!received_time) {
    log_message("no valid NTP response was received\n");
    return 1;
  }

  char target_text[64] = {};
  if (format_utc(network_time, target_text, sizeof(target_text))) {
    log_message("NTP time: %s\n", target_text);
  }

  bool updated = set_system_time(network_time);
  if (!updated) {
    return 1;
  }

  log_message("date/time synchronization completed\n");
  return 0;
}

}  // namespace

int main() {
  int result = synchronize_system_time();
  notify_user();
  return result;
}
