# K-FSW Communications

Communications for K-FSW: the CSP stack, its interfaces and the router. It
uses [libcsp](https://github.com/libcsp/libcsp), the CubeSat Space Protocol
library.

libcsp is a separate west project at `third_party/libcsp`, pinned by the
composition to `097a039701c85e4ceb98e91f380810662e23878a`.
`zephyr/module.yml` declares the Zephyr module.

Two kinds of link go through the same router, and the routes decide which one
reaches each destination:

```text
  UART / KISS   a serial line, or a radio that behaves like one
  CAN           libcsp splits packets across CAN frames
```

Three nodes on one link, with the router passing each packet on:

![Traffic between three nodes over CSP](https://raw.githubusercontent.com/dgonzalez97/k-fsw/main/docs/media/param-over-a-link.gif)

Full documentation is on the [K-FSW site](https://dgonzalez97.github.io/k-fsw/).

## CAN

A CAN frame holds eight bytes, so libcsp splits CSP packets over several
frames. This repository chooses the controller and the bitrate and starts it.

The controller comes from a `kfsw,csp-can` chosen node, so the shared code has
no board or peripheral names:

```text
  board   can1 ----------+
                         +-- libcsp CAN driver -- CSP interface "CAN"
  host    native-linux --+
          CAN -> SocketCAN, for a USB adapter
```

The bitrate can be 125k, 250k, 500k, 800k or 1M. Changing it restarts the
controller, and every node on the bus needs the same bitrate, so change the
other nodes as well. `kfsw_can_set_bitrate()` only reports whether the
controller accepted the value.

The controller accepts every frame. libcsp filters by the CFP header, and a
routing node has to see traffic for other nodes too.

## Several UART/KISS interfaces

A single-link composition uses a `kfsw,csp-uart` chosen node, named `KISS`, and
gets a `0/0 KISS` route when `KFSW_CSP_ROUTE_TABLE` is empty.

With several links, the UARTs are children of one `kfsw,csp-kiss-uarts` node.
Each child has its own UART, interface name, address, prefix length, framing
state and counters:

```dts
/ {
    csp-kiss-uarts {
        compatible = "kfsw,csp-kiss-uarts";

        ground {
            uart = <&uart1>;
            interface-name = "KISS_1";
            address = <8>;
            prefix-length = <14>;
        };

        payload {
            uart = <&uart2>;
            interface-name = "KISS_2";
            address = <9>;
            prefix-length = <14>;
        };
    };
};
```

Names must be unique and one to nine characters from ASCII letters, digits,
`_` and `-`, and each child needs a different UART. Nine characters is the
libcsp route parser's limit, so `KISS_1` works and `KISS_GROUND` doesn't.
Polling profiles need `CSP_UART_RX_THREAD_NUM` to be at least the number of
children; interrupt-driven profiles have a callback and KISS context per UART.

## Static routes

`KFSW_CSP_ROUTE_TABLE` is passed to the libcsp parser unchanged. It is a
comma-separated list:

```text
destination[/prefix-length] interface [via], next-entry
10/14 KISS_1,11/14 KISS_2 11
```

CSP v2 node IDs are 14 bits and the prefix length counts those bits: `/14` is
one node, `/0` is the default route, and no prefix means `/14`. The longest
matching prefix wins. Two entries with the same destination and prefix are both
used; the second one is not a fallback.

The optional last number is the link-layer next hop, and direct routes store
`CSP_NO_VIA_ADDRESS`. KISS has no link-layer address and ignores it, but K-FSW
keeps it and shows it.

At startup K-FSW registers the interfaces, rejects a table longer than the
parser's 99 characters, runs `csp_rtable_check()`, checks the table size and
then loads it. An unknown interface or a bad entry fails initialization, and if
the loaded table doesn't match the checked one the table is cleared.

Routes can't be changed at runtime or saved. `kfsw_csp_route_table_check()`
checks a table without loading it, and `kfsw_csp_visit_routes()` lists the
routes.

## Packet buffers

K-FSW follows libcsp's zero-copy rules:

- a packet from a receive call has to be freed or passed to a send or reply
  call;
- a packet passed to a send call is freed by libcsp, also when sending fails,
  so the caller must not use or free it again;
- interfaces pass complete packets to the router queue, which routes or frees
  them;
- when the pool or a queue is full, the allocation fails or the packet is
  dropped and counted. Nothing retries forever or grows without a limit.

## License

Licensed under [Apache 2.0](LICENSE). Third-party dependencies retain their
own licences.
