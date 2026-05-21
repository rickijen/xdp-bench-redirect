# DPDK testpmd AF_XDP Multi-Flow Reserved-Queue Benchmark - 2026-05-21

## Executive Summary

I ran another 60-second AF_XDP multi-flow benchmark using the safer reserved-queue model validated earlier:

- RX queue 0 stayed reserved for kernel/control-plane traffic.
- TCP/22 was steered to queue 0 with an `ethtool` ntuple rule.
- RSS excluded queue 0, so benchmark UDP was distributed only across queues 1..N.
- DPDK AF_XDP PMD bound only queues 1..N with `start_queue=1`.
- The XDP program remained fail-open for non-benchmark traffic.

Both directions completed the full 60-second benchmark with fresh SSH checks passing during the run. This is the important safety improvement over the previous full-queue AF_XDP multi-flow attempt, where reverse direction caused SSH timeouts on the receiving host.

## Key Result

| Direction | TX burst | TX packets | RX packets | RX Mpps | RX L2 Gbps | RX L1 Gbps | Delivery | RX queue spread | Mid-run SSH |
|---|---:|---:|---:|---:|---:|---:|---:|---|---|
| `10.46.68.57 -> 10.46.69.45` | 32 | 2,108,897,088 | 2,107,711,994 | 35.1285 | 17.9859 | 23.6064 | 99.9438% | 31/31 material | pass |
| `10.46.69.45 -> 10.46.68.57` | 32 | 3,258,318,752 | 1,980,850,371 | 33.0142 | 16.9033 | 22.1855 | 60.7936% | 15/15 material | pass |

L1 bandwidth uses 64-byte packets plus 20 bytes preamble/IPG:

```text
RX L1 Gbps = RX pps * (64 + 20) * 8
```

## Hosts

| Host | Interface | MAC | Combined queues | AF_XDP queues used | Control queue |
|---|---|---|---:|---:|---:|
| `1u1g-x570-0020` / `10.46.68.57` | `enp1s0f0np0` | `08:c0:eb:ca:2a:d2` | 16 | 1-15 | 0 |
| `a1u1g-mil-0406` / `10.46.69.45` | `ens65f0np0` | `e8:eb:d3:ef:7f:16` | 32 | 1-31 | 0 |

CPU layout:

| Host | DPDK lcores | Forwarding lcores | Management/control CPUs |
|---|---|---|---|
| `10.46.68.57` | `2-7,10-15` | `3-7,10-15` | `0,1,8,9`; control lcore `2` |
| `10.46.69.45` | `2-15,18-31` | `3-15,18-31` | `0,1,16,17`; control lcore `2` |

## Safety Model Used

The reserved-queue model keeps SSH out of the AF_XDP queue set:

```text
Ingress packet
  -> NIC hardware steering/RSS
     -> TCP/22: queue 0
     -> benchmark UDP RSS: queues 1..N
  -> XDP_DRV program
     -> exact benchmark UDP match: XDP_REDIRECT to AF_XDP socket on queue 1..N
     -> SSH/TCP/ARP/ICMP/other traffic: XDP_PASS to kernel stack
```

Active steering state before the run:

| Host | Ntuple | RSS |
|---|---|---|
| `10.46.68.57` | TCP/22 destination port -> queue 0 | queue 0 excluded, queues 1-15 used |
| `10.46.69.45` | TCP/22 destination port -> queue 0 | queue 0 excluded, queues 1-31 used |

The XDP redirect program was the same reviewed multi-flow object:

```text
source: xdp/xdp_bench_redirect.c
source sha256: 3a19a4349b92afae1c978230af105430392a1b1cccbf9c83318a76d5f5ef9884
remote object: /tmp/afxdp_bench/xdp_bench_redirect_multiflow.o
remote object sha256: 574f03d2292919f3ec2b86808ba613861171e7a9537b64d0c8a3a0d6eab04833
```

## Detailed Steps Performed

1. Established SSH control sessions to both hosts as `local-rjen`.

```text
10.46.68.57 / 1u1g-x570-0020
10.46.69.45 / a1u1g-mil-0406
```

2. Confirmed the host, NIC, queue, and CPU layout used by the run.

```text
10.46.68.57:
  interface: enp1s0f0np0
  MAC:       08:c0:eb:ca:2a:d2
  queues:    16 combined
  CPUs:      8 physical / 16 logical

10.46.69.45:
  interface: ens65f0np0
  MAC:       e8:eb:d3:ef:7f:16
  queues:    32 combined
  CPUs:      16 physical / 32 logical
```

3. Used the reviewed XDP source and the on-host compiled BPF object from the earlier validation work.

```text
source path:  xdp/xdp_bench_redirect.c
object path:  /tmp/afxdp_bench/xdp_bench_redirect_multiflow.o
program tag:  ffa6d38fefd7da06
```

The source was reviewed for fail-open behavior before benchmarking:

```text
redirect only exact benchmark UDP traffic
match destination MAC for the receiving host
match IPv4 and UDP only
match benchmark source/destination IPs
match benchmark destination UDP port
allow only the expected DPDK multi-flow source-port pattern
return XDP_PASS for TCP/22, ARP, ICMP, fragments, malformed packets, and all other traffic
redirect only when an AF_XDP socket exists for ctx->rx_queue_index
```

4. Validated the BPF object before traffic.

```bash
bpftool prog dump xlated
```

The translated dump was checked for the expected redirect path, including the `xsk_map_redirect` call, and the program was attached in native driver mode during smoke validation.

5. Enabled hardware ntuple steering for SSH/control traffic.

On `.68`:

```bash
sudo ethtool -K enp1s0f0np0 ntuple on
sudo ethtool -N enp1s0f0np0 flow-type tcp4 dst-port 22 action 0 loc 1
```

On `.69`:

```bash
sudo ethtool -K ens65f0np0 ntuple on
sudo ethtool -N ens65f0np0 flow-type tcp4 dst-port 22 action 0 loc 1
```

6. Changed RSS indirection so benchmark traffic would not land on queue 0.

On `.68`:

```bash
sudo ethtool -X enp1s0f0np0 weight 0 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1
```

On `.69`:

```bash
sudo ethtool -X ens65f0np0 weight 0 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1 1
```

7. Validated the safer AF_XDP queue binding before the full benchmark.

```text
10.46.68.57: start_queue=1, queue_count=15, --rxq=15 --txq=15
10.46.69.45: start_queue=1, queue_count=31, --rxq=31 --txq=31
```

During validation:

```text
XDP attached in driver mode
queue 0 remained outside the AF_XDP socket set
TCP/22 ntuple steering remained installed
RSS continued to exclude queue 0
fresh SSH checks passed
```

8. Ran a high-rate smoke test in the previously unsafe direction, `.69 -> .68`.

```text
sender:   10.46.69.45, queues 1-31, burst 32
receiver: 10.46.68.57, queues 1-15
result:   SSH to the .68 receiver stayed reachable during traffic
```

9. Started the full forward benchmark, `.68 -> .69`.

```text
receiver first: 10.46.69.45, AF_XDP queues 1-31
sender second:  10.46.68.57, AF_XDP queues 1-15
traffic:        txonly multi-flow, 64-byte packets, burst 32
duration:       60 seconds
```

During the run, fresh SSH checks were made to both the sender and receiver. After 60 seconds, `testpmd` was stopped and port statistics plus per-queue counters were collected.

10. Cleaned up the forward direction processes before starting reverse direction.

```text
stopped dpdk-testpmd
detached XDP
confirmed SSH remained reachable
left queue-0 steering in place
```

11. Started the full reverse benchmark, `.69 -> .68`.

```text
receiver first: 10.46.68.57, AF_XDP queues 1-15
sender second:  10.46.69.45, AF_XDP queues 1-31
traffic:        txonly multi-flow, 64-byte packets, burst 32
duration:       60 seconds
```

Fresh SSH checks were again made to both hosts during traffic. After 60 seconds, `testpmd` was stopped and TX/RX/queue statistics were collected.

12. Performed final cleanup and host-state validation.

```text
XDP detached on both interfaces
dpdk-testpmd stopped on both hosts
promiscuous mode disabled on both interfaces
rollback timers killed
SSH reachable on both hosts
TCP/22 queue-0 steering intentionally left active for follow-up runs
RSS queue-0 exclusion intentionally left active for follow-up runs
```

## Commands Used

Forward receiver, `.69`:

```bash
sudo dpdk-testpmd -l 2-15,18-31 -n 4 --no-pci --huge-unlink=always \
  --file-prefix=afxrun4rx69 \
  --vdev=net_af_xdp0,iface=ens65f0np0,start_queue=1,queue_count=31,xdp_prog=/tmp/afxdp_bench/xdp_bench_redirect_multiflow.o \
  -- --nb-cores=27 --rxq=31 --txq=31 --rss-udp -i --disable-device-start --port-topology=chained
```

Forward sender, `.68`:

```bash
sudo dpdk-testpmd -l 2-7,10-15 -n 4 --no-pci --huge-unlink=always \
  --file-prefix=afxrun4tx68 \
  --vdev=net_af_xdp0,iface=enp1s0f0np0,start_queue=1,queue_count=15,xdp_prog=/tmp/afxdp_bench/xdp_bench_redirect_multiflow.o \
  -- --nb-cores=11 --rxq=15 --txq=15 --txonly-multi-flow --rss-udp -i --disable-device-start \
  --port-topology=chained --eth-peer=0,e8:eb:d3:ef:7f:16 \
  --tx-ip=198.18.57.1,198.18.69.1 --tx-udp=49152,49153
```

Reverse receiver, `.68`:

```bash
sudo dpdk-testpmd -l 2-7,10-15 -n 4 --no-pci --huge-unlink=always \
  --file-prefix=afxrun4rx68 \
  --vdev=net_af_xdp0,iface=enp1s0f0np0,start_queue=1,queue_count=15,xdp_prog=/tmp/afxdp_bench/xdp_bench_redirect_multiflow.o \
  -- --nb-cores=11 --rxq=15 --txq=15 --rss-udp -i --disable-device-start --port-topology=chained
```

Reverse sender, `.69`:

```bash
sudo dpdk-testpmd -l 2-15,18-31 -n 4 --no-pci --huge-unlink=always \
  --file-prefix=afxrun4tx69 \
  --vdev=net_af_xdp0,iface=ens65f0np0,start_queue=1,queue_count=31,xdp_prog=/tmp/afxdp_bench/xdp_bench_redirect_multiflow.o \
  -- --nb-cores=27 --rxq=31 --txq=31 --txonly-multi-flow --rss-udp -i --disable-device-start \
  --port-topology=chained --eth-peer=0,08:c0:eb:ca:2a:d2 \
  --tx-ip=198.18.69.1,198.18.57.1 --tx-udp=49154,49155
```

Interactive `testpmd` settings on the TX side:

```text
port start 0
set fwd txonly
set txpkts 64
set burst 32
clear port stats all
start
```

## Forward Run Details

Direction:

```text
10.46.68.57 enp1s0f0np0 -> 10.46.69.45 ens65f0np0
```

TX side:

```text
TX-packets:         2,108,897,088
TX-errors:            774,511,616
TX-bytes:         134,969,413,632
testpmd TX-dropped: 1,549,023,232
```

RX side:

```text
RX-packets: 2,107,711,994
RX-missed:  0
RX-errors:  0
RX-nombuf:  0
RX-bytes:   134,894,412,668
RX-dropped: 0
```

Derived:

```text
TX rate:      35.1483 Mpps
RX rate:      35.1285 Mpps
RX L2:        17.9859 Gbps
RX L1:        23.6064 Gbps
Delivery:     99.9438%
```

Mid-run SSH checks:

```text
10.46.68.57: run4_fwd_mid_tx68_ssh_ok_2026-05-21T15:44:51Z
10.46.69.45: run4_fwd_mid_rx69_ssh_ok_2026-05-21T15:44:55Z
```

Receiver `.69` AF_XDP queue distribution, local DPDK queues 0-30 mapping to kernel queues 1-31:

```text
Q0   65,866,518
Q1   65,866,409
Q2   65,866,701
Q3   72,843,081
Q4   65,579,263
Q5   69,110,524
Q6   65,266,487
Q7   73,443,675
Q8   64,979,128
Q9   66,754,018
Q10  64,979,184
Q11  75,789,591
Q12  66,153,506
Q13  62,622,673
Q14  66,466,625
Q15  75,197,858
Q16  65,866,614
Q17  65,864,767
Q18  65,866,568
Q19  75,200,036
Q20  66,466,663
Q21  62,622,798
Q22  66,153,725
Q23  74,913,000
Q24  66,754,078
Q25  64,979,308
Q26  66,754,125
Q27  72,553,938
Q28  65,264,288
Q29  69,110,667
Q30  72,556,178
```

CPU observation:

```text
10.46.68.57 sender average:   25.19% user,  2.75% sys, 32.99% soft, 39.06% idle
10.46.69.45 receiver average:  8.86% user, 75.56% sys,  0.03% soft, 15.55% idle
```

On `.69`, forwarding cores were mostly around 87-91% system time. CPU 0, the reserved control queue CPU, stayed 99.70% idle during the sample.

## Reverse Run Details

Direction:

```text
10.46.69.45 ens65f0np0 -> 10.46.68.57 enp1s0f0np0
```

TX side:

```text
TX-packets:          3,258,318,752
TX-errors:          21,990,969,568
TX-bytes:          208,532,400,128
testpmd TX-dropped: 43,981,939,136
```

RX side:

```text
RX-packets: 1,980,850,371
RX-missed:  0
RX-errors:  0
RX-nombuf:  0
RX-bytes:   126,774,483,896
RX-dropped: 0
```

Derived:

```text
TX rate:      54.3053 Mpps
RX rate:      33.0142 Mpps
RX L2:        16.9033 Gbps
RX L1:        22.1855 Gbps
Delivery:     60.7936%
```

Mid-run SSH checks:

```text
10.46.68.57: run4_rev_mid_rx68_ssh_ok_2026-05-21T15:48:13Z
10.46.69.45: run4_rev_mid_tx69_ssh_ok_2026-05-21T15:48:11Z
```

Receiver `.68` AF_XDP queue distribution, local DPDK queues 0-14 mapping to kernel queues 1-15:

```text
Q0   129,709,548
Q1   131,018,634
Q2   135,710,054
Q3   129,944,782
Q4   129,819,895
Q5   130,978,538
Q6   135,717,909
Q7   129,622,105
Q8   129,750,636
Q9   131,176,307
Q10  135,534,691
Q11  129,236,513
Q12  129,924,610
Q13  131,107,666
Q14  141,598,483
```

CPU observation:

```text
10.46.68.57 receiver average:  9.37% user, 52.39% sys,  8.93% soft, 29.30% idle
10.46.69.45 sender average:   30.70% user,  1.00% sys, 37.54% soft, 30.77% idle
```

On `.68`, the forwarding cores were saturated in system/softirq time, while the control CPUs remained mostly available: CPU 0 was 96.29% idle, CPU 1 was 99.43% idle, and CPU 2 was 99.77% idle.

## Comparison To Prior Runs

| Direction | Prior full-queue AF_XDP multi-flow | Reserved-queue AF_XDP multi-flow |
|---|---:|---:|
| `.68 -> .69` | 33.8043 Mpps, burst 4, SSH pass | 35.1285 Mpps, burst 32, SSH pass |
| `.69 -> .68` | no valid result; SSH timed out even at burst 1 | 33.0142 Mpps, burst 32, SSH pass |

Compared with the `mlx5` multicore run3 baseline:

| Direction | `mlx5` run3 RX | AF_XDP reserved-queue RX | Delta |
|---|---:|---:|---:|
| `.68 -> .69` | 32.6426 Mpps | 35.1285 Mpps | +7.62% |
| `.69 -> .68` | 39.6843 Mpps | 33.0142 Mpps | -16.81% |

Compared with exact-flow AF_XDP:

| Direction | Exact-flow AF_XDP RX | Multi-flow reserved-queue AF_XDP RX |
|---|---:|---:|
| `.68 -> .69` | 1.1357 Mpps | 35.1285 Mpps |
| `.69 -> .68` | 0.9475 Mpps | 33.0142 Mpps |

## PMD Comparison Metrics

For comparing `mlx5` PMD against AF_XDP PMD, the primary metric should be RX-side packet rate:

```text
Primary metric: RX Mpps
```

This is the cleanest PMD comparison for this benchmark because packet size is fixed at 64 bytes and the central question is how many packets per second the receive path actually processed.

The best companion line-rate metric is:

```text
Secondary metric: RX L1 Gbps
```

`RX L1 Gbps` estimates physical wire consumption by adding preamble and inter-packet gap overhead to the 64-byte frame size. This is the better metric for judging how close the received traffic is to line rate.

`RX L2 Gbps` is still useful, but it represents the DPDK-visible Ethernet frame byte rate. It excludes physical-layer overhead, so it is less useful for line-rate comparison.

TX packets or TX Gbps should not be used as the main PMD comparison in this benchmark. The AF_XDP sender can overgenerate and report large TX errors/drops. The `.69 -> .68` run is the clearest example: `.69` generated far more packets than `.68` could receive, so RX Mpps is the meaningful throughput number.

Recommended comparison set:

```text
Primary:   RX Mpps
Secondary: RX L1 Gbps
Sanity:    RX-missed, RX-errors, RX-nombuf, delivery percentage
```

## Analysis

The reserved-queue model achieved the main goal: both benchmark directions completed for 60 seconds while SSH stayed reachable on both hosts. The earlier reverse-direction failure was not caused by the XDP program dropping SSH; SSH was `XDP_PASS`, but full-queue AF_XDP plus high packet rate starved the host enough that SSH became unreliable. Reserving queue 0 and excluding it from AF_XDP fixed that operational risk.

The benchmark is now an `N-1` queue benchmark rather than an all-queues benchmark. That is the right tradeoff for a shared management/benchmark NIC. Queue 0 remains available for SSH and kernel traffic; AF_XDP consumes only benchmark UDP from queues 1..N.

The queue distribution is materially better than the original exact 5-tuple AF_XDP run. All AF_XDP receive queues carried traffic in both directions, which is why throughput moved from roughly 1 Mpps to roughly 33-35 Mpps.

The receiver side was clean in both directions: `RX-missed`, `RX-errors`, and `RX-nombuf` stayed at zero. The high TX error/drop counters are on the generator side, where `testpmd --txonly-multi-flow` produced more packets than the AF_XDP/netdev TX path could enqueue. This is especially visible on `.69 -> .68`, where the sender generated far beyond the receiver's sustainable rate; RX throughput is the more meaningful benchmark number.

The `.69 -> .68` delivery percentage was only 60.7936% because `.69` overgenerated relative to what `.68` could receive through AF_XDP. The sender reported 3,258,318,752 TX packets, while the receiver reported 1,980,850,371 RX packets:

```text
1,980,850,371 / 3,258,318,752 = 60.7936%
```

That low delivery ratio is not explained by receiver-side `testpmd` drops, since `.68` reported `RX-missed: 0`, `RX-errors: 0`, and `RX-nombuf: 0`. The stronger signal is the `.69` sender pressure: 21,990,969,568 `TX-errors` and 43,981,939,136 `testpmd TX-dropped`. In practice, the larger `.69` host, using 31 AF_XDP benchmark queues, can drive the AF_XDP TX path much harder than the smaller `.68` host can drain with 15 AF_XDP RX queues. Packets are therefore lost before or around the transmit/driver/network path rather than appearing as receiver-side mbuf or NIC RX misses.

Directional asymmetry remains. `.68 -> .69` slightly exceeded the prior `mlx5` run3 forward result, while `.69 -> .68` landed below the `mlx5` run3 reverse result. The reverse direction is likely limited by the smaller `.68` host as AF_XDP receiver: it has 15 benchmark RX queues after reserving queue 0 and fewer CPUs than `.69`.

## Final Host State

After the benchmark:

```text
XDP detached on both interfaces
dpdk-testpmd stopped on both hosts
promiscuous mode disabled on both interfaces
rollback timers killed
SSH reachable on both hosts
```

The safe hardware steering state was intentionally left active for follow-up runs:

```text
TCP/22 ntuple rule -> queue 0
RSS excludes queue 0
```

To restore default steering later:

```bash
# 10.46.68.57
sudo ip link set dev enp1s0f0np0 xdp off 2>/dev/null || true
sudo ethtool -X enp1s0f0np0 default
sudo ethtool -N enp1s0f0np0 delete 1 2>/dev/null || true
sudo ethtool -K enp1s0f0np0 ntuple off

# 10.46.69.45
sudo ip link set dev ens65f0np0 xdp off 2>/dev/null || true
sudo ethtool -X ens65f0np0 default
sudo ethtool -N ens65f0np0 delete 1 2>/dev/null || true
sudo ethtool -K ens65f0np0 ntuple off
```
