# xdp-bench-redirect

Fail-open XDP redirect program for a DPDK AF_XDP benchmark.

The program redirects only the benchmark UDP flows into an AF_XDP `XSKMAP`
named `xsks_map`. Everything else returns `XDP_PASS`, including SSH/TCP,
ARP, ICMP, non-benchmark UDP traffic, parser failures, fragmented IPv4, and
missing AF_XDP sockets.

## Benchmark Flows

The program matches these two flows:

| Direction | Destination MAC | Source IP | Destination IP | Source UDP | Destination UDP |
| --- | --- | --- | --- | --- | --- |
| `10.10.0.10 -> 10.10.0.11` | `5e:9f:1b:3c:4d:2a` | `198.18.57.1` | `198.18.69.1` | `49152` | `49153` |
| `10.10.0.11 -> 10.10.0.10` | `5a:3f:1b:22:8c:4e` | `198.18.69.1` | `198.18.57.1` | `49154` | `49155` |

## Install Build Tools

On Ubuntu:

```bash
sudo apt-get update
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y \
  clang llvm llvm-dev libbpf-dev libelf-dev make bpftool
```

## Build

Compile on the host that will load the program:

```bash
clang -O2 -g -Wall -Wextra -target bpf -D__TARGET_ARCH_x86 \
  -I/usr/include/x86_64-linux-gnu \
  -c xdp_bench_redirect.c \
  -o xdp_bench_redirect.o
```

Confirm the object:

```bash
file xdp_bench_redirect.o
readelf -s xdp_bench_redirect.o | grep -E 'xdp_bench_redirect|xsks_map|license'
sha256sum xdp_bench_redirect.c xdp_bench_redirect.o
```

## Validate Before Benchmark Traffic

Attach one host at a time, with a rollback timer already armed:

```bash
IFACE=enp1s0f0np0
OBJ=xdp_bench_redirect.o

sudo ip link set dev "$IFACE" xdp off 2>/dev/null || true
sudo nohup sh -c "sleep 90; ip link set dev '$IFACE' xdp off" >/tmp/xdp-rollback.log 2>&1 &
sudo ip link set dev "$IFACE" xdpdrv obj "$OBJ" sec xdp
```

Verify native driver mode:

```bash
sudo bpftool net
ip -details link show dev "$IFACE"
```

Verify the loaded program and translated instructions:

```bash
PROG_ID=$(sudo bpftool net | sed -n "s/^$IFACE.* id \([0-9][0-9]*\).*$/\1/p")
sudo bpftool prog show id "$PROG_ID"
sudo bpftool map show | grep -E 'xsks_map|xskmap'
sudo bpftool prog dump xlated id "$PROG_ID"
```

Open a fresh SSH session while the program is attached. Do not run benchmark
traffic unless SSH works and `bpftool net` reports XDP in `driver` mode.

Detach after validation:

```bash
sudo ip link set dev "$IFACE" xdp off
sudo bpftool net
```

## DPDK AF_XDP Usage

The DPDK AF_XDP PMD should be started with this object via `xdp_prog=...` so
the PMD populates the `xsks_map` sockets:

```bash
--vdev=net_af_xdp0,iface=<iface>,start_queue=<queue>,queue_count=<count>,xdp_prog=/path/to/xdp_bench_redirect.o
```

Only queues that have AF_XDP sockets in `xsks_map` can receive redirected
packets. If there is no socket for `ctx->rx_queue_index`, this program returns
`XDP_PASS`.
