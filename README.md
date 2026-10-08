# K-FSW Communications

Communications for K-FSW: the CSP stack, its interfaces and the router. It
uses [libcsp](https://github.com/libcsp/libcsp), the CubeSat Space Protocol
library.

The composition pins the [K-FSW libcsp fork](https://github.com/dgonzalez97/kfsw-libcsp)
at `third_party/libcsp`. Check `k-fsw/west.yml` for the revision and the fork's
`KFSW.md` for its changes. `zephyr/module.yml` declares the Zephyr module.

Routes select a UART/KISS or CAN interface for each destination:

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
sets `KFSW_CSP_ROUTE_TABLE="0/0 KISS"`.

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
10 KISS_1,11 KISS_2 11
```

An empty table loads `0/0 LOOP`, so a node built without one reaches only
itself. Links are named by the composition, never picked by start order.

The prefix length counts the bits of a node ID: 14 in CSP 2, 5 in CSP 1
(`CONFIG_KFSW_CSP_VERSION_1`). The full width is one node, `/0` is the default
route, and no prefix means the full width, so a route without one works in both
versions. The longest
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

## Radio packet protection

`KFSW_CSP_UART_CODEC` lets the radio module protect one named KISS interface.
The module owns the key, sessions and encryption settings. Comms queues
received packets for decoding outside the UART interrupt handler and passes
accepted packets to the router.

Transmit encoding runs in the sending thread. The codec keeps libcsp's KISS
framing and packet ownership; it does not protect other interfaces.

## Diagnostics

`kfsw_csp_visit_interfaces()` reads local interface counters.
`kfsw_csp_interface_stats_read()` queries a named interface on another node
through CMP.
Counts can wrap and change during a read. A timeout does not distinguish an
unknown interface from a lost request or reply.

## Packet buffers

K-FSW follows libcsp's zero-copy rules:

- a packet from a receive call has to be freed or passed to a send or reply
  call;
- a packet passed to a send call is freed by libcsp, also when sending fails,
  so the caller must not use or free it again;
- interfaces pass complete packets to the router queue, which routes or frees
  them;
- when the pool or a queue is full, the allocation fails or the packet is
  dropped and counted.

## License

Licensed under [Apache 2.0](LICENSE). Third-party dependencies retain their
own licences.
