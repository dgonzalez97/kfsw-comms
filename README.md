# K-FSW Communications

K-FSW communications owns the common communication stacks, interfaces, and
routing lifecycle used by the application. Its first implementation integrates
[libcsp](https://github.com/libcsp/libcsp) for Cubesat Space Protocol routing.

The composition repository pins libcsp to commit
`097a039701c85e4ceb98e91f380810662e23878a` and supplies the target-specific
Kconfig values. Native PTY profiles use libcsp's Zephyr USART driver. Physical
UART profiles use Zephyr interrupt-driven receive with libcsp's maintained KISS
decoder and transmitter. K-FSW configures each Zephyr device, registers each
independently named KISS interface, loads libcsp's native static routing table,
and exposes status and end-to-end test APIs. CAN remains intentionally
deferred.

libcsp remains a standalone west project so the composition workspace can pin
one exact upstream revision without vendoring its source. Its checkout lives at
`kfsw-comms/third_party/libcsp`, making the dependency's ownership visible in
the workspace layout while preserving its independent Git history.
`kfsw-comms/zephyr/module.yml` declares the module dependency, while this
repository owns how K-FSW configures libcsp and exposes its transport APIs.

## Multiple UART/KISS interfaces

The legacy one-link composition remains a `kfsw,csp-uart` chosen node named
`KISS`. It receives the same direct `0/0 KISS` route when
`KFSW_CSP_ROUTE_TABLE` is empty.

A multi-link composition declares any number of enabled children under one
`kfsw,csp-kiss-uarts` node. The child owns its UART phandle, interface name,
address, prefix length, framing state, transport context, and counters:

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

Names must be unique, contain one through nine ASCII letters, digits, `_`, or
`-`, and reference a unique UART. Nine characters is the pinned libcsp text
parser's limit, so `KISS_1` is valid while `KISS_GROUND` is not. Polling
profiles must set `CSP_UART_RX_THREAD_NUM` to at least the number of children;
interrupt-driven profiles keep independent callback/KISS contexts instead.

## Static routes

`KFSW_CSP_ROUTE_TABLE` uses the pinned libcsp parser directly. Its format is a
comma-separated list:

```text
destination[/prefix-length] interface [via], next-entry
10/14 KISS_1,11/14 KISS_2 11
```

CSP v2 has a 14-bit node ID. The mask is a prefix length over those 14 bits:
`/14` is one exact node and `/0` is the default route. Omitting the mask means
`/14`. libcsp chooses the longest matching prefix. If entries have identical
destination and prefix, the pinned implementation sends through each eligible
matching interface rather than treating them as fallback priorities.

The optional final integer is libcsp's link-layer next-hop (`via`). Direct
routes store `CSP_NO_VIA_ADDRESS`. libcsp passes a configured next hop to the
selected interface. The pinned KISS driver has no link-layer address and
therefore intentionally ignores it, but K-FSW retains and reports the field.

At startup K-FSW registers every interface, rejects tables longer than the
pinned parser's 99-character limit, calls `csp_rtable_check()` over the entire
table, checks capacity, and only then calls `csp_rtable_load()`. Unknown
interface names and malformed entries fail initialization. A load discrepancy
clears the table rather than exposing a partial configuration. Runtime route
mutation and persistent route storage are deliberately not exposed; callers
can validate without mutation through `kfsw_csp_route_table_check()` and
inspect snapshots through `kfsw_csp_visit_routes()`.

## Packet ownership

K-FSW follows libcsp's zero-copy ownership rules directly:

- A packet returned by a CSP receive API belongs to the receiver until it is
  freed or passed to a CSP send/reply API.
- A packet passed to a CSP send API transfers ownership, including when the
  interface reports a transmit failure; callers must not reuse or free it.
- Interface receive paths transfer complete packets to CSP's router queue.
  libcsp either queues, routes, or frees them.
- Pool or queue exhaustion causes allocation failure or a counted packet drop.
  K-FSW-owned code does not retry indefinitely or allocate an unbounded queue.
