# PS5 Time & Date Sync by Deckerr97

A lightweight, standalone PS5 payload that restores the console's system date
and time using public NTP servers.

It is intended for consoles with a depleted, disconnected, or faulty CMOS/RTC
battery where the clock resets after the PS5 is completely powered off.

## Download

Download the latest precompiled `ps5-date-time-sync.elf` from the
[Releases](https://github.com/kerrdec97/ps5-date-time-sync/releases/latest)
page.

## Features

- Automatically retrieves the current date and time from public NTP servers.
- Uses Cloudflare, `pool.ntp.org`, and Google Public NTP.
- Tries Cloudflare's published numeric NTP addresses first, allowing clock
  recovery even when DNS is unavailable on the PS5.
- Validates the reply endpoint, NTP version and mode, stratum, request
  timestamp, and accepted date range before changing the clock.
- Reads the clock back after the update and rejects failed verification.
- Preserves the console's existing timezone setting.
- Displays one notification per execution:

  > PS5 time & date sync by Deckerr97

- Exits automatically after synchronization.
- Does not install an application or modify files on the console.
- Is fully standalone and independent of other payloads.

## Usage

1. Connect the PS5 to the Internet.
2. Inject or launch `ps5-date-time-sync.elf` with a compatible payload loader.
3. Wait for the notification.
4. The payload attempts to synchronize the clock and then exits automatically.

If the clock resets after a complete shutdown, launch the payload once after
booting the console.

## Requirements

- A compatible PS5 exploit and ELF payload loader.
- An active Internet connection.
- Outbound UDP port 123 access for NTP.
- An execution environment that permits `settimeofday`.

The numeric Cloudflare fallbacks remove the initial dependency on DNS. If the
router, firewall, or Internet provider blocks UDP port 123, synchronization
will still fail.

## Compatibility

The release ELF was built with a PS5 Payload SDK package containing firmware
definitions through PS5 firmware 13.60.

This does not guarantee operation on every firmware. Runtime compatibility
depends on the exploit, ELF loader, firmware-specific restrictions, and
whether the active environment permits system-clock modification.

## Building

Install the [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk), set
`PS5_PAYLOAD_SDK` to its installation directory, and run:

```sh
make
```

The output will be:

```text
ps5-date-time-sync.elf
```

To send it through a normal ELF loader:

```sh
make test PS5_HOST=192.168.0.233
```

To upload and launch it through Payload Manager:

```sh
make manager-deploy PS5_HOST=192.168.0.233
```

## Notification and logs

The on-screen notification is intentionally kept simple. Detailed success and
failure information is written to the payload output instead.

If the notification appears more than once after a single intended launch,
the payload manager or ELF loader has most likely launched the ELF multiple
times.

## How it works

```text
Launch payload
      |
      v
Initialize network
      |
      v
Try numeric Cloudflare NTP addresses
      |
      +-- fall back to hostname-based providers if needed
      |
      v
Request and validate NTP time
      |
      v
Update and verify the PS5 system clock
      |
      v
Write details to the payload output
      |
      v
Show one notification and exit
```

## Acknowledgements

Special thanks to **John Törnblom** and every contributor to the
[PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk) for creating and
maintaining the toolchain that made this payload possible.

Thanks also to the public time-service providers used by the payload:
[Cloudflare Time Services](https://developers.cloudflare.com/time-services/ntp/),
the [NTP Pool Project](https://www.ntppool.org/), and
[Google Public NTP](https://developers.google.com/time).

## Disclaimer

Use this software at your own risk. This project is not affiliated with or
endorsed by Sony Interactive Entertainment.

---

Created by **Deckerr97**.
