# K-FSW Communications

Everything to do with getting a packet from one node to another: the stacks,
the interfaces, and the routing lifecycle. The current implementation is
[libcsp](https://github.com/libcsp/libcsp), for Cubesat Space Protocol.

libcsp stays a separate west project, checked out at
`third_party/libcsp` and pinned by the composition to
`097a039701c85e4ceb98e91f380810662e23878a`. Pinning an exact upstream revision
without vendoring the source keeps the dependency's history its own and its
ownership visible in the workspace. `zephyr/module.yml` declares the module;
this repository owns how K-FSW configures it and what it exposes.

Two links are supported, both behind the same router, so a route decides which
one a destination takes:

```text
  UART / KISS   a serial line, or a radio that looks like one
  CAN           libcsp fragments a packet across frames with its own protocol
```

Three nodes on one link — a ground node, a board and a second host node — with
the router deciding where each packet goes:

![Traffic between three nodes over CSP](https://raw.githubusercontent.com/dgonzalez97/k-fsw/main/docs/media/param-over-a-link.gif)

Full documentation is on the
[K-FSW site](https://dgonzalez97.github.io/k-fsw/); what follows is the
reasoning behind the parts that are easy to get wrong.

## CAN

A CAN frame carries eight bytes, so libcsp fragments a CSP packet across
several of them with its own protocol. That is libcsp's job; this repository
owns which controller is used, at what bitrate, and when it starts.

The controller comes from a `kfsw,csp-can` chosen node, so reusable code names
no board and no peripheral:

```text
  board        can1  ──┐
                       ├── same libcsp driver ── CSP interface "CAN"
  host    native-linux ┘         │
          CAN → SocketCAN        └── a ground node reaches a USB adapter
                                     without a second implementation
```

Only 125k, 250k, 500k, 800k and 1M are selectable. Changing the bitrate stops
and restarts the controller, and since both ends of a bus must agree, a node
reconfigured on its own goes quiet until whatever is at the other end follows.
That is the caller's to sequence; `kfsw_can_set_bitrate()` only reports whether
the controller accepted it.

The controller-level filter accepts everything. A routing node wants that:
libcsp decides what belongs to it from the CFP header, and a hardware filter
would drop traffic the node is meant to forward.

## Several UART/KISS interfaces

A one-link composition is a `kfsw,csp-uart` chosen node named `KISS`. It gets
the direct `0/0 KISS` route when `KFSW_CSP_ROUTE_TABLE` is empty.

A multi-link composition declares enabled children under one
`kfsw,csp-kiss-uarts` node. Each child owns its UART phandle, interface name,
address, prefix length, framing state, transport context and counters:

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

Names must be unique, one to nine characters of ASCII letters, digits, `_` or
`-`, and each must reference a different UART. Nine is the pinned text parser's
limit, which is why `KISS_1` works and `KISS_GROUND` does not. Polling profiles
need `CSP_UART_RX_THREAD_NUM` at least as large as the number of children;
interrupt-driven profiles keep independent callback and KISS contexts instead.

## Static routes

`KFSW_CSP_ROUTE_TABLE` is handed to the pinned libcsp parser as-is. The format
is a comma-separated list:

```text
destination[/prefix-length] interface [via], next-entry
10/14 KISS_1,11/14 KISS_2 11
```

CSP v2 node IDs are 14 bits, and the mask is a prefix length over those bits:
`/14` is one exact node, `/0` is the default route, and omitting it means
`/14`. libcsp picks the longest matching prefix. Entries with an identical
destination and prefix are not fallbacks — the pinned implementation sends
through every eligible interface.

The optional trailing integer is the link-layer next hop. Direct routes store
`CSP_NO_VIA_ADDRESS`. The pinned KISS driver has no link-layer address and
ignores a next hop, but K-FSW keeps and reports the field.

At startup K-FSW registers the interfaces, rejects a table longer than the
parser's 99-character limit, runs `csp_rtable_check()` over the whole thing,
checks capacity, and only then loads it. An unknown interface name or a
malformed entry fails initialisation, and a load that does not match what was
checked clears the table rather than leaving a partial configuration in place.

Routes cannot be changed at runtime or stored persistently. Callers can
validate without mutating through `kfsw_csp_route_table_check()` and inspect
through `kfsw_csp_visit_routes()`.

## Packet ownership

K-FSW follows libcsp's zero-copy rules directly:

- a packet from a receive API belongs to the receiver until it is freed or
  passed to a send or reply API;
- a packet passed to a send API is gone, including when the interface reports a
  transmit failure — callers must not reuse or free it;
- interface receive paths hand complete packets to the router queue, which
  queues, routes or frees them;
- an exhausted pool or queue fails the allocation or counts a drop. Nothing
  here retries forever or grows a queue without a bound.

## License

Licensed under [Apache 2.0](LICENSE). Third-party dependencies retain their
own licences.
