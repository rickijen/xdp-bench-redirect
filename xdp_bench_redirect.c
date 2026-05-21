// SPDX-License-Identifier: GPL-2.0
#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <bpf/bpf_endian.h>
#include <bpf/bpf_helpers.h>
#include <stdbool.h>

#define MAX_XSKS 256

#define IPV4_ADDR(a, b, c, d) \
	bpf_htonl(((__u32)(a) << 24) | ((__u32)(b) << 16) | ((__u32)(c) << 8) | (__u32)(d))

#define IP_57 IPV4_ADDR(198, 18, 57, 1)
#define IP_69 IPV4_ADDR(198, 18, 69, 1)

#define UDP_57_SRC bpf_htons(49152)
#define UDP_57_DST bpf_htons(49153)
#define UDP_69_SRC bpf_htons(49154)
#define UDP_69_DST bpf_htons(49155)

#define UDP_SRC_MULTI_FLOW_MIN 49152

#ifndef IP_MF
#define IP_MF 0x2000
#endif
#ifndef IP_OFFSET
#define IP_OFFSET 0x1fff
#endif

struct {
	__uint(type, BPF_MAP_TYPE_XSKMAP);
	__uint(max_entries, MAX_XSKS);
	__type(key, __u32);
	__type(value, __u32);
} xsks_map SEC(".maps");

static __always_inline bool mac_is_57(const unsigned char *mac)
{
	return mac[0] == 0x08 && mac[1] == 0xc0 && mac[2] == 0xeb &&
	       mac[3] == 0xca && mac[4] == 0x2a && mac[5] == 0xd2;
}

static __always_inline bool mac_is_69(const unsigned char *mac)
{
	return mac[0] == 0xe8 && mac[1] == 0xeb && mac[2] == 0xd3 &&
	       mac[3] == 0xef && mac[4] == 0x7f && mac[5] == 0x16;
}

static __always_inline int redirect_to_xsk(struct xdp_md *ctx)
{
	__u32 qid = ctx->rx_queue_index;

	if (qid >= MAX_XSKS)
		return XDP_PASS;
	if (!bpf_map_lookup_elem(&xsks_map, &qid))
		return XDP_PASS;

	return bpf_redirect_map(&xsks_map, qid, XDP_PASS);
}

static __always_inline bool lcore_is_57_sender(__u8 lcore)
{
	return (lcore >= 3 && lcore <= 7) || (lcore >= 10 && lcore <= 15);
}

static __always_inline bool lcore_is_69_sender(__u8 lcore)
{
	return (lcore >= 3 && lcore <= 15) || (lcore >= 18 && lcore <= 31);
}

static __always_inline bool sport_is_57_sender_multiflow(__u16 sport)
{
	if (sport < UDP_SRC_MULTI_FLOW_MIN)
		return false;

	return lcore_is_57_sender((__u8)(sport & 0xff));
}

static __always_inline bool sport_is_69_sender_multiflow(__u16 sport)
{
	if (sport < UDP_SRC_MULTI_FLOW_MIN)
		return false;

	return lcore_is_69_sender((__u8)(sport & 0xff));
}

SEC("xdp")
int xdp_bench_redirect(struct xdp_md *ctx)
{
	void *data_end = (void *)(long)ctx->data_end;
	void *data = (void *)(long)ctx->data;
	struct ethhdr *eth = data;
	struct iphdr *iph;
	__u32 ihl;
	bool dst_57;
	bool dst_69;

	if ((void *)(eth + 1) > data_end)
		return XDP_PASS;

	dst_57 = mac_is_57(eth->h_dest);
	dst_69 = mac_is_69(eth->h_dest);
	if (!dst_57 && !dst_69)
		return XDP_PASS;

	if (eth->h_proto != bpf_htons(ETH_P_IP))
		return XDP_PASS;

	iph = (void *)(eth + 1);
	if ((void *)(iph + 1) > data_end)
		return XDP_PASS;
	if (iph->version != 4)
		return XDP_PASS;

	ihl = iph->ihl * 4;
	if (ihl < sizeof(*iph))
		return XDP_PASS;
	if ((void *)iph + ihl > data_end)
		return XDP_PASS;
	if (iph->frag_off & bpf_htons(IP_MF | IP_OFFSET))
		return XDP_PASS;

	if (iph->protocol == IPPROTO_TCP) {
		struct tcphdr *tcp = (void *)iph + ihl;

		if ((void *)(tcp + 1) > data_end)
			return XDP_PASS;
		if (tcp->source == bpf_htons(22) || tcp->dest == bpf_htons(22))
			return XDP_PASS;
		return XDP_PASS;
	}

	if (iph->protocol == IPPROTO_UDP) {
		struct udphdr *udp = (void *)iph + ihl;
		__u16 sport;

		if ((void *)(udp + 1) > data_end)
			return XDP_PASS;

		sport = bpf_ntohs(udp->source);

		if (dst_69 && iph->saddr == IP_57 && iph->daddr == IP_69 &&
		    udp->dest == UDP_57_DST &&
		    (udp->source == UDP_57_SRC || sport_is_57_sender_multiflow(sport)))
			return redirect_to_xsk(ctx);

		if (dst_57 && iph->saddr == IP_69 && iph->daddr == IP_57 &&
		    udp->dest == UDP_69_DST &&
		    (udp->source == UDP_69_SRC || sport_is_69_sender_multiflow(sport)))
			return redirect_to_xsk(ctx);
	}

	return XDP_PASS;
}

char _license[] SEC("license") = "GPL";
