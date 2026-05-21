# xdp-bench-redirect

Fail-open XDP redirect program for the reserved-queue DPDK AF_XDP multi-flow
benchmark.

The program redirects only the benchmark UDP flows into the AF_XDP `XSKMAP`
named `xsks_map`. Everything else returns `XDP_PASS`, including SSH/TCP,
ARP, ICMP, non-benchmark UDP traffic, parser failures, fragmented IPv4, and
missing AF_XDP sockets.

## Current Source

This repository intentionally contains one C source file:

```text
xdp_bench_redirect.c
```

For the latest reserved-queue AF_XDP multi-flow benchmark, this source was the
only C source used. On each benchmark host it was copied as:

```text
/tmp/afxdp_bench/xdp_bench_redirect_multiflow.c
```

and compiled to:

```text
/tmp/afxdp_bench/xdp_bench_redirect_multiflow.o
```

Recorded hashes from the benchmark:

```text
xdp_bench_redirect.c:
  3a19a4349b92afae1c978230af105430392a1b1cccbf9c83318a76d5f5ef9884

xdp_bench_redirect_multiflow.o:
  574f03d2292919f3ec2b86808ba613861171e7a9537b64d0c8a3a0d6eab04833
```

## Reserved-Queue Safety Model

The benchmark NIC is shared by benchmark traffic and SSH management traffic, so
queue 0 is reserved for kernel/control-plane traffic:

```text
Ingress packet
  -> NIC hardware steering/RSS
     -> TCP/22: queue 0
     -> benchmark UDP RSS: queues 1..N
  -> XDP_DRV program
     -> benchmark UDP match: XDP_REDIRECT to AF_XDP socket on queue 1..N
     -> SSH/TCP/ARP/ICMP/other traffic: XDP_PASS to kernel stack
```

DPDK AF_XDP must bind only benchmark queues:

```text
10.10.0.10: start_queue=1, queue_count=15, --rxq=15 --txq=15
10.10.0.11: start_queue=1, queue_count=31, --rxq=31 --txq=31
```

Do not use `start_queue=0` while SSH shares the benchmark NIC.

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
mkdir -p /tmp/afxdp_bench
cp xdp_bench_redirect.c /tmp/afxdp_bench/xdp_bench_redirect_multiflow.c

clang -O2 -g -Wall -Wextra -target bpf -D__TARGET_ARCH_x86 \
  -I/usr/include/x86_64-linux-gnu \
  -c /tmp/afxdp_bench/xdp_bench_redirect_multiflow.c \
  -o /tmp/afxdp_bench/xdp_bench_redirect_multiflow.o
```

Confirm the object:

```bash
file /tmp/afxdp_bench/xdp_bench_redirect_multiflow.o
readelf -s /tmp/afxdp_bench/xdp_bench_redirect_multiflow.o | \
  grep -E 'xdp_bench_redirect|xsks_map|license'
sha256sum /tmp/afxdp_bench/xdp_bench_redirect_multiflow.c \
  /tmp/afxdp_bench/xdp_bench_redirect_multiflow.o
```

## Validate Before Benchmark Traffic

Attach one host at a time, with a rollback timer already armed:

```bash
IFACE=enp1s0f0np0
OBJ=/tmp/afxdp_bench/xdp_bench_redirect_multiflow.o

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

The translated dump should show the XSKMAP redirect path. Open a fresh SSH
session while the program is attached. Do not run benchmark traffic unless SSH
works and `bpftool net` reports XDP in `driver` mode.

Detach after validation:

```bash
sudo ip link set dev "$IFACE" xdp off
sudo bpftool net
```

## Configure Reserved Queue Steering

On `.68`:

```bash
sudo ethtool -K enp1s0f0np0 ntuple on
sudo ethtool -N enp1s0f0np0 flow-type tcp4 dst-port 22 action 0 loc 1
sudo ethtool -X enp1s0f0np0 weight 0 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1
```

On `.69`:

```bash
sudo ethtool -K ens65f0np0 ntuple on
sudo ethtool -N ens65f0np0 flow-type tcp4 dst-port 22 action 0 loc 1
sudo ethtool -X ens65f0np0 weight 0 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1
```

## DPDK AF_XDP Usage

The DPDK AF_XDP PMD should be started with this object via `xdp_prog=...` so
the PMD populates the `xsks_map` sockets:

```bash
--vdev=net_af_xdp0,iface=<iface>,start_queue=1,queue_count=<count>,xdp_prog=/tmp/afxdp_bench/xdp_bench_redirect_multiflow.o
```

Only queues that have AF_XDP sockets in `xsks_map` can receive redirected
packets. If there is no socket for `ctx->rx_queue_index`, this program returns
`XDP_PASS`.

Receiver on `.69`:

```bash
sudo dpdk-testpmd -l 2-15,18-31 -n 4 --no-pci --huge-unlink=always \
  --file-prefix=afxrx69 \
  --vdev=net_af_xdp0,iface=ens65f0np0,start_queue=1,queue_count=31,xdp_prog=/tmp/afxdp_bench/xdp_bench_redirect_multiflow.o \
  -- --nb-cores=27 --rxq=31 --txq=31 --rss-udp -i --disable-device-start --port-topology=chained
```

Sender on `.68`:

```bash
sudo dpdk-testpmd -l 2-7,10-15 -n 4 --no-pci --huge-unlink=always \
  --file-prefix=afxtx68 \
  --vdev=net_af_xdp0,iface=enp1s0f0np0,start_queue=1,queue_count=15,xdp_prog=/tmp/afxdp_bench/xdp_bench_redirect_multiflow.o \
  -- --nb-cores=11 --rxq=15 --txq=15 --txonly-multi-flow --rss-udp -i --disable-device-start \
  --port-topology=chained --eth-peer=0,5e:9f:1b:3c:4d:2a \
  --tx-ip=198.18.57.1,198.18.69.1 --tx-udp=49152,49153
```

Interactive TX settings:

```text
port start 0
set fwd txonly
set txpkts 64
set burst 32
clear port stats all
start
```

Reverse direction uses the analogous sender settings:

```text
--eth-peer=0,5a:3f:1b:22:8c:4e
--tx-ip=198.18.69.1,198.18.57.1
--tx-udp=49154,49155
```

## Restore Defaults

On `.68`:

```bash
sudo ip link set dev enp1s0f0np0 xdp off 2>/dev/null || true
sudo ethtool -X enp1s0f0np0 default
sudo ethtool -N enp1s0f0np0 delete 1 2>/dev/null || true
sudo ethtool -K enp1s0f0np0 ntuple off
```

On `.69`:

```bash
sudo ip link set dev ens65f0np0 xdp off 2>/dev/null || true
sudo ethtool -X ens65f0np0 default
sudo ethtool -N ens65f0np0 delete 1 2>/dev/null || true
sudo ethtool -K ens65f0np0 ntuple off
```
