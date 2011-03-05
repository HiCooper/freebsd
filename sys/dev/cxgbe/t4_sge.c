/*-
 * Copyright (c) 2011 Chelsio Communications, Inc.
 * All rights reserved.
 * Written by: Navdeep Parhar <np@FreeBSD.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include "opt_inet.h"

#include <sys/types.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/queue.h>
#include <sys/taskqueue.h>
#include <sys/sysctl.h>
#include <net/bpf.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_vlan_var.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>

#include "common/common.h"
#include "common/t4_regs.h"
#include "common/t4_regs_values.h"
#include "common/t4_msg.h"
#include "common/t4fw_interface.h"

struct fl_buf_info {
	int size;
	int type;
	uma_zone_t zone;
};

/* t4_sge_init will fill up the zone */
static struct fl_buf_info fl_buf_info[FL_BUF_SIZES] = {
	{ MCLBYTES, EXT_CLUSTER, NULL},
	{ MJUMPAGESIZE, EXT_JUMBOP, NULL},
	{ MJUM9BYTES, EXT_JUMBO9, NULL},
	{ MJUM16BYTES, EXT_JUMBO16, NULL}
};
#define FL_BUF_SIZE(x)	(fl_buf_info[x].size)
#define FL_BUF_TYPE(x)	(fl_buf_info[x].type)
#define FL_BUF_ZONE(x)	(fl_buf_info[x].zone)

enum {
	FL_PKTSHIFT = 2
};

#define FL_ALIGN	min(CACHE_LINE_SIZE, 32)
#if CACHE_LINE_SIZE > 64
#define SPG_LEN		128
#else
#define SPG_LEN		64
#endif

/* Used to track coalesced tx work request */
struct txpkts {
	uint64_t *flitp;	/* ptr to flit where next pkt should start */
	uint8_t npkt;		/* # of packets in this work request */
	uint8_t nflits;		/* # of flits used by this work request */
	uint16_t plen;		/* total payload (sum of all packets) */
};

/* A packet's SGL.  This + m_pkthdr has all info needed for tx */
struct sgl {
	int nsegs;		/* # of segments in the SGL, 0 means imm. tx */
	int nflits;		/* # of flits needed for the SGL */
	bus_dma_segment_t seg[TX_SGL_SEGS];
};

static inline void init_iq(struct sge_iq *, struct adapter *, int, int, int,
    int, iq_intr_handler_t *, char *);
static inline void init_fl(struct sge_fl *, int, char *);
static inline void init_txq(struct sge_txq *, int, char *);
static int alloc_ring(struct adapter *, size_t, bus_dma_tag_t *, bus_dmamap_t *,
    bus_addr_t *, void **);
static int free_ring(struct adapter *, bus_dma_tag_t, bus_dmamap_t, bus_addr_t,
    void *);
static int alloc_iq_fl(struct port_info *, struct sge_iq *, struct sge_fl *,
    int);
static int free_iq_fl(struct port_info *, struct sge_iq *, struct sge_fl *);
static int alloc_iq(struct sge_iq *, int);
static int free_iq(struct sge_iq *);
static int alloc_rxq(struct port_info *, struct sge_rxq *, int, int);
static int free_rxq(struct port_info *, struct sge_rxq *);
static int alloc_txq(struct port_info *, struct sge_txq *, int);
static int free_txq(struct port_info *, struct sge_txq *);
static void oneseg_dma_callback(void *, bus_dma_segment_t *, int, int);
static inline bool is_new_response(const struct sge_iq *, struct rsp_ctrl **);
static inline void iq_next(struct sge_iq *);
static inline void ring_fl_db(struct adapter *, struct sge_fl *);
static void refill_fl(struct sge_fl *, int);
static int alloc_fl_sdesc(struct sge_fl *);
static void free_fl_sdesc(struct sge_fl *);
static int alloc_eq_maps(struct sge_eq *);
static void free_eq_maps(struct sge_eq *);
static struct mbuf *get_fl_sdesc_data(struct sge_fl *, int, int);
static void set_fl_tag_idx(struct sge_fl *, int);

static int get_pkt_sgl(struct sge_txq *, struct mbuf **, struct sgl *, int);
static int free_pkt_sgl(struct sge_txq *, struct sgl *);
static int write_txpkt_wr(struct port_info *, struct sge_txq *, struct mbuf *,
    struct sgl *);
static int add_to_txpkts(struct port_info *, struct sge_txq *, struct txpkts *,
    struct mbuf *, struct sgl *);
static void write_txpkts_wr(struct sge_txq *, struct txpkts *);
static inline void write_ulp_cpl_sgl(struct port_info *, struct sge_txq *,
    struct txpkts *, struct mbuf *, struct sgl *);
static int write_sgl_to_txd(struct sge_eq *, struct sgl *, caddr_t *);
static inline void copy_to_txd(struct sge_eq *, caddr_t, caddr_t *, int);
static inline void ring_tx_db(struct adapter *, struct sge_eq *);
static int reclaim_tx_descs(struct sge_eq *, int, int);
static void write_eqflush_wr(struct sge_eq *);
static __be64 get_flit(bus_dma_segment_t *, int, int);
static int handle_sge_egr_update(struct adapter *,
    const struct cpl_sge_egr_update *);

/**
 *	t4_sge_init - initialize SGE
 *	@sc: the adapter
 *
 *	Performs SGE initialization needed every time after a chip reset.
 *	We do not initialize any of the queues here, instead the driver
 *	top-level must request them individually.
 */
void
t4_sge_init(struct adapter *sc)
{
	struct sge *s = &sc->sge;
	int i;

	FL_BUF_ZONE(0) = zone_clust;
	FL_BUF_ZONE(1) = zone_jumbop;
	FL_BUF_ZONE(2) = zone_jumbo9;
	FL_BUF_ZONE(3) = zone_jumbo16;

	t4_set_reg_field(sc, A_SGE_CONTROL, V_PKTSHIFT(M_PKTSHIFT) |
			 V_INGPADBOUNDARY(M_INGPADBOUNDARY) |
			 F_EGRSTATUSPAGESIZE,
			 V_INGPADBOUNDARY(ilog2(FL_ALIGN) - 5) |
			 V_PKTSHIFT(FL_PKTSHIFT) |
			 F_RXPKTCPLMODE |
			 V_EGRSTATUSPAGESIZE(SPG_LEN == 128));
	t4_set_reg_field(sc, A_SGE_HOST_PAGE_SIZE,
			 V_HOSTPAGESIZEPF0(M_HOSTPAGESIZEPF0),
			 V_HOSTPAGESIZEPF0(PAGE_SHIFT - 10));

	for (i = 0; i < FL_BUF_SIZES; i++) {
		t4_write_reg(sc, A_SGE_FL_BUFFER_SIZE0 + (4 * i),
		    FL_BUF_SIZE(i));
	}

	t4_write_reg(sc, A_SGE_INGRESS_RX_THRESHOLD,
		     V_THRESHOLD_0(s->counter_val[0]) |
		     V_THRESHOLD_1(s->counter_val[1]) |
		     V_THRESHOLD_2(s->counter_val[2]) |
		     V_THRESHOLD_3(s->counter_val[3]));

	t4_write_reg(sc, A_SGE_TIMER_VALUE_0_AND_1,
		     V_TIMERVALUE0(us_to_core_ticks(sc, s->timer_val[0])) |
		     V_TIMERVALUE1(us_to_core_ticks(sc, s->timer_val[1])));
	t4_write_reg(sc, A_SGE_TIMER_VALUE_2_AND_3,
		     V_TIMERVALUE2(us_to_core_ticks(sc, s->timer_val[2])) |
		     V_TIMERVALUE3(us_to_core_ticks(sc, s->timer_val[3])));
	t4_write_reg(sc, A_SGE_TIMER_VALUE_4_AND_5,
		     V_TIMERVALUE4(us_to_core_ticks(sc, s->timer_val[4])) |
		     V_TIMERVALUE5(us_to_core_ticks(sc, s->timer_val[5])));
}

int
t4_create_dma_tag(struct adapter *sc)
{
	int rc;

	rc = bus_dma_tag_create(bus_get_dma_tag(sc->dev), 1, 0,
	    BUS_SPACE_MAXADDR, BUS_SPACE_MAXADDR, NULL, NULL, BUS_SPACE_MAXSIZE,
	    BUS_SPACE_UNRESTRICTED, BUS_SPACE_MAXSIZE, BUS_DMA_ALLOCNOW, NULL,
	    NULL, &sc->dmat);
	if (rc != 0) {
		device_printf(sc->dev,
		    "failed to create main DMA tag: %d\n", rc);
	}

	return (rc);
}

int
t4_destroy_dma_tag(struct adapter *sc)
{
	if (sc->dmat)
		bus_dma_tag_destroy(sc->dmat);

	return (0);
}

/*
 * Allocate and initialize the firmware event queue and the forwarded interrupt
 * queues, if any.  The adapter owns all these queues as they are not associated
 * with any particular port.
 *
 * Returns errno on failure.  Resources allocated up to that point may still be
 * allocated.  Caller is responsible for cleanup in case this function fails.
 */
int
t4_setup_adapter_iqs(struct adapter *sc)
{
	int i, rc;
	struct sge_iq *iq, *fwq;
	iq_intr_handler_t *handler;
	char name[16];

	ADAPTER_LOCK_ASSERT_NOTOWNED(sc);

	fwq = &sc->sge.fwq;
	if (sc->flags & INTR_FWD) {
		iq = &sc->sge.fiq[0];

		/*
		 * Forwarded interrupt queues - allocate 1 if there's only 1
		 * vector available, one less than the number of vectors
		 * otherwise (the first vector is reserved for the error
		 * interrupt in that case).
		 */
		i = sc->intr_count > 1 ? 1 : 0;
		for (; i < sc->intr_count; i++, iq++) {

			snprintf(name, sizeof(name), "%s fiq%d",
			    device_get_nameunit(sc->dev), i);
			init_iq(iq, sc, 0, 0, (sc->sge.nrxq + 1) * 2, 16, NULL,
			    name);

			rc = alloc_iq(iq, i);
			if (rc != 0) {
				device_printf(sc->dev,
				    "failed to create fwd intr queue %d: %d\n",
				    i, rc);
				return (rc);
			}
		}

		handler = t4_intr_evt;
		i = 0;	/* forward fwq's interrupt to the first fiq */
	} else {
		handler = NULL;
		i = 1;	/* fwq should use vector 1 (0 is used by error) */
	}

	snprintf(name, sizeof(name), "%s fwq", device_get_nameunit(sc->dev));
	init_iq(fwq, sc, 0, 0, FW_IQ_QSIZE, FW_IQ_ESIZE, handler, name);
	rc = alloc_iq(fwq, i);
	if (rc != 0) {
		device_printf(sc->dev,
		    "failed to create firmware event queue: %d\n", rc);
	}

	return (rc);
}

/*
 * Idempotent
 */
int
t4_teardown_adapter_iqs(struct adapter *sc)
{
	int i;
	struct sge_iq *iq;

	ADAPTER_LOCK_ASSERT_NOTOWNED(sc);

	iq = &sc->sge.fwq;
	free_iq(iq);
	if (sc->flags & INTR_FWD) {
		for (i = 0; i < NFIQ(sc); i++) {
			iq = &sc->sge.fiq[i];
			free_iq(iq);
		}
	}

	return (0);
}

int
t4_setup_eth_queues(struct port_info *pi)
{
	int rc = 0, i, intr_idx;
	struct sge_rxq *rxq;
	struct sge_txq *txq;
	char name[16];
	struct adapter *sc = pi->adapter;

	if (sysctl_ctx_init(&pi->ctx) == 0) {
		struct sysctl_oid *oid = device_get_sysctl_tree(pi->dev);
		struct sysctl_oid_list *children = SYSCTL_CHILDREN(oid);

		pi->oid_rxq = SYSCTL_ADD_NODE(&pi->ctx, children, OID_AUTO,
		    "rxq", CTLFLAG_RD, NULL, "rx queues");
		pi->oid_txq = SYSCTL_ADD_NODE(&pi->ctx, children, OID_AUTO,
		    "txq", CTLFLAG_RD, NULL, "tx queues");
	}

	for_each_rxq(pi, i, rxq) {

		snprintf(name, sizeof(name), "%s rxq%d-iq",
		    device_get_nameunit(pi->dev), i);
		init_iq(&rxq->iq, sc, pi->tmr_idx, pi->pktc_idx,
		    pi->qsize_rxq, RX_IQ_ESIZE,
		    sc->flags & INTR_FWD ? t4_intr_data: NULL, name);

		snprintf(name, sizeof(name), "%s rxq%d-fl",
		    device_get_nameunit(pi->dev), i);
		init_fl(&rxq->fl, pi->qsize_rxq / 8, name);

		if (sc->flags & INTR_FWD)
			intr_idx = (pi->first_rxq + i) % NFIQ(sc);
		else
			intr_idx = pi->first_rxq + i + 2;

		rc = alloc_rxq(pi, rxq, intr_idx, i);
		if (rc != 0)
			goto done;

		intr_idx++;
	}

	for_each_txq(pi, i, txq) {

		snprintf(name, sizeof(name), "%s txq%d",
		    device_get_nameunit(pi->dev), i);
		init_txq(txq, pi->qsize_txq, name);

		rc = alloc_txq(pi, txq, i);
		if (rc != 0)
			goto done;
	}

done:
	if (rc)
		t4_teardown_eth_queues(pi);

	return (rc);
}

/*
 * Idempotent
 */
int
t4_teardown_eth_queues(struct port_info *pi)
{
	int i;
	struct sge_rxq *rxq;
	struct sge_txq *txq;

	/* Do this before freeing the queues */
	if (pi->oid_txq || pi->oid_rxq) {
		sysctl_ctx_free(&pi->ctx);
		pi->oid_txq = pi->oid_rxq = NULL;
	}

	for_each_txq(pi, i, txq) {
		free_txq(pi, txq);
	}

	for_each_rxq(pi, i, rxq) {
		free_rxq(pi, rxq);
	}

	return (0);
}

/* Deals with errors and forwarded interrupts */
void
t4_intr_all(void *arg)
{
	struct adapter *sc = arg;

	t4_intr_err(arg);
	t4_intr_fwd(&sc->sge.fiq[0]);
}

/* Deals with forwarded interrupts on the given ingress queue */
void
t4_intr_fwd(void *arg)
{
	struct sge_iq *iq = arg, *q;
	struct adapter *sc = iq->adapter;
	struct rsp_ctrl *ctrl;
	int ndesc_pending = 0, ndesc_total = 0;
	int qid;

	IQ_LOCK(iq);
	while (is_new_response(iq, &ctrl)) {

		rmb();

		/* Only interrupt muxing expected on this queue */
		KASSERT(G_RSPD_TYPE(ctrl->u.type_gen) == X_RSPD_TYPE_INTR,
		    ("unexpected event on forwarded interrupt queue: %x",
		    G_RSPD_TYPE(ctrl->u.type_gen)));

		qid = ntohl(ctrl->pldbuflen_qid) - sc->sge.iq_start;
		q = sc->sge.iqmap[qid];

		q->handler(q);

		ndesc_total++;
		if (++ndesc_pending >= iq->qsize / 4) {
			t4_write_reg(sc, MYPF_REG(A_SGE_PF_GTS),
			    V_CIDXINC(ndesc_pending) |
			    V_INGRESSQID(iq->cntxt_id) |
			    V_SEINTARM(
				V_QINTR_TIMER_IDX(X_TIMERREG_UPDATE_CIDX)));
			ndesc_pending = 0;
		}

		iq_next(iq);
	}
	IQ_UNLOCK(iq);

	if (ndesc_total > 0) {
		t4_write_reg(sc, MYPF_REG(A_SGE_PF_GTS),
		    V_CIDXINC(ndesc_pending) | V_INGRESSQID((u32)iq->cntxt_id) |
		    V_SEINTARM(iq->intr_params));
	}
}

/* Deals with error interrupts */
void
t4_intr_err(void *arg)
{
	struct adapter *sc = arg;

	if (sc->intr_type == 1)
		t4_write_reg(sc, MYPF_REG(A_PCIE_PF_CLI), 0);

	t4_slow_intr_handler(sc);
}

/* Deals with the firmware event queue */
void
t4_intr_evt(void *arg)
{
	struct sge_iq *iq = arg;
	struct adapter *sc = iq->adapter;
	struct rsp_ctrl *ctrl;
	const struct rss_header *rss;
	int ndesc_pending = 0, ndesc_total = 0;

	KASSERT(iq == &sc->sge.fwq, ("%s: unexpected ingress queue", __func__));

	IQ_LOCK(iq);
	while (is_new_response(iq, &ctrl)) {

		rmb();

		rss = (const void *)iq->cdesc;

		/* Should only get CPL on this queue */
		KASSERT(G_RSPD_TYPE(ctrl->u.type_gen) == X_RSPD_TYPE_CPL,
		    ("%s: unexpected type %d", __func__,
		    G_RSPD_TYPE(ctrl->u.type_gen)));

		switch (rss->opcode) {
		case CPL_FW4_MSG:
		case CPL_FW6_MSG: {
			const struct cpl_fw6_msg *cpl;

			cpl = (const void *)(rss + 1);
			if (cpl->type == FW6_TYPE_CMD_RPL)
				t4_handle_fw_rpl(sc, cpl->data);

			break;
			}
		case CPL_SGE_EGR_UPDATE:
			handle_sge_egr_update(sc, (const void *)(rss + 1));
			break;

		default:
			device_printf(sc->dev,
			    "can't handle CPL opcode %d.", rss->opcode);
		}

		ndesc_total++;
		if (++ndesc_pending >= iq->qsize / 4) {
			t4_write_reg(sc, MYPF_REG(A_SGE_PF_GTS),
			    V_CIDXINC(ndesc_pending) |
			    V_INGRESSQID(iq->cntxt_id) |
			    V_SEINTARM(
				V_QINTR_TIMER_IDX(X_TIMERREG_UPDATE_CIDX)));
			ndesc_pending = 0;
		}
		iq_next(iq);
	}
	IQ_UNLOCK(iq);

	if (ndesc_total > 0) {
		t4_write_reg(sc, MYPF_REG(A_SGE_PF_GTS),
		    V_CIDXINC(ndesc_pending) | V_INGRESSQID(iq->cntxt_id) |
		    V_SEINTARM(iq->intr_params));
	}
}

void
t4_intr_data(void *arg)
{
	struct sge_rxq *rxq = arg;
	struct sge_iq *iq = arg;
	struct rsp_ctrl *ctrl;
	struct sge_fl *fl = &rxq->fl;
	struct port_info *pi = rxq->port;
	struct ifnet *ifp = pi->ifp;
	struct adapter *sc = pi->adapter;
	const struct rss_header *rss;
	const struct cpl_rx_pkt *cpl;
	int ndescs = 0, rsp_type;
	uint32_t len;
	struct mbuf *m0, *m;
#ifdef INET
	struct lro_ctrl *lro = &rxq->lro;
	struct lro_entry *l;
#endif

	IQ_LOCK(iq);
	iq->intr_next = iq->intr_params;
	while (is_new_response(iq, &ctrl)) {

		rmb();

		rss = (const void *)iq->cdesc;
		cpl = (const void *)(rss + 1);

		rsp_type = G_RSPD_TYPE(ctrl->u.type_gen);

		if (__predict_false(rsp_type == X_RSPD_TYPE_CPL)) {
			/* Can't be anything except an egress update */
			handle_sge_egr_update(sc, (const void *)cpl);
			goto nextdesc;
		}

		KASSERT(G_RSPD_TYPE(ctrl->u.type_gen) == X_RSPD_TYPE_FLBUF,
		    ("unexpected event on data ingress queue: %x",
		    G_RSPD_TYPE(ctrl->u.type_gen)));

		len = be32toh(ctrl->pldbuflen_qid);

		KASSERT(len & F_RSPD_NEWBUF,
		    ("%s: T4 misconfigured to pack buffers.", __func__));

		len = G_RSPD_LEN(len);
		m0 = get_fl_sdesc_data(fl, len, M_PKTHDR);
		if (m0 == NULL) {
			iq->intr_next = V_QINTR_TIMER_IDX(SGE_NTIMERS - 1);
			break;
		}

		len -= FL_PKTSHIFT;
		m0->m_len -= FL_PKTSHIFT;
		m0->m_data += FL_PKTSHIFT;

		m0->m_pkthdr.len = len;
		m0->m_pkthdr.rcvif = ifp;
		m0->m_flags |= M_FLOWID;
		m0->m_pkthdr.flowid = rss->hash_val;

		if (cpl->csum_calc && !cpl->err_vec &&
		    ifp->if_capenable & IFCAP_RXCSUM) {
			m0->m_pkthdr.csum_flags |= (CSUM_IP_CHECKED |
			    CSUM_IP_VALID | CSUM_DATA_VALID | CSUM_PSEUDO_HDR);
			if (cpl->ip_frag)
				m0->m_pkthdr.csum_data = be16toh(cpl->csum);
			else
				m0->m_pkthdr.csum_data = 0xffff;
			rxq->rxcsum++;
		}

		if (cpl->vlan_ex) {
			m0->m_pkthdr.ether_vtag = be16toh(cpl->vlan);
			m0->m_flags |= M_VLANTAG;
			rxq->vlan_extraction++;
		}

		len -= m0->m_len;
		m = m0;
		while (len) {
			m->m_next = get_fl_sdesc_data(fl, len, 0);
			if (m->m_next == NULL)
				CXGBE_UNIMPLEMENTED("mbuf recovery");

			m = m->m_next;
			len -= m->m_len;
		}
#ifdef INET
		if (cpl->l2info & htobe32(F_RXF_LRO) &&
		    rxq->flags & RXQ_LRO_ENABLED &&
		    tcp_lro_rx(lro, m0, 0) == 0) {
			/* queued for LRO */
		} else
#endif
			(*ifp->if_input)(ifp, m0);

		FL_LOCK(fl);
		if (fl->needed >= 32) {
			refill_fl(fl, 64);
			if (fl->pending >= 32)
				ring_fl_db(sc, fl);
		}
		FL_UNLOCK(fl);

nextdesc:	ndescs++;
		iq_next(iq);

		if (ndescs > 32) {
			t4_write_reg(sc, MYPF_REG(A_SGE_PF_GTS),
			    V_CIDXINC(ndescs) |
			    V_INGRESSQID((u32)iq->cntxt_id) |
			    V_SEINTARM(V_QINTR_TIMER_IDX(X_TIMERREG_UPDATE_CIDX)));
			ndescs = 0;
		}
	}

#ifdef INET
	while (!SLIST_EMPTY(&lro->lro_active)) {
		l = SLIST_FIRST(&lro->lro_active);
		SLIST_REMOVE_HEAD(&lro->lro_active, next);
		tcp_lro_flush(lro, l);
	}
#endif

	t4_write_reg(sc, MYPF_REG(A_SGE_PF_GTS), V_CIDXINC(ndescs) |
	    V_INGRESSQID((u32)iq->cntxt_id) | V_SEINTARM(iq->intr_next));

	IQ_UNLOCK(iq);

	FL_LOCK(fl);
	if (fl->needed) {
		refill_fl(fl, -1);
		if (fl->pending >= 8)
			ring_fl_db(sc, fl);
	}
	FL_UNLOCK(fl);
}

/* Per-packet header in a coalesced tx WR, before the SGL starts (in flits) */
#define TXPKTS_PKT_HDR ((\
    sizeof(struct ulp_txpkt) + \
    sizeof(struct ulptx_idata) + \
    sizeof(struct cpl_tx_pkt_core) \
    ) / 8)

/* Header of a coalesced tx WR, before SGL of first packet (in flits) */
#define TXPKTS_WR_HDR (\
    sizeof(struct fw_eth_tx_pkts_wr) / 8 + \
    TXPKTS_PKT_HDR)

/* Header of a tx WR, before SGL of first packet (in flits) */
#define TXPKT_WR_HDR ((\
    sizeof(struct fw_eth_tx_pkt_wr) + \
    sizeof(struct cpl_tx_pkt_core) \
    ) / 8 )

/* Header of a tx LSO WR, before SGL of first packet (in flits) */
#define TXPKT_LSO_WR_HDR ((\
    sizeof(struct fw_eth_tx_pkt_wr) + \
    sizeof(struct cpl_tx_pkt_lso) + \
    sizeof(struct cpl_tx_pkt_core) \
    ) / 8 )

int
t4_eth_tx(struct ifnet *ifp, struct sge_txq *txq, struct mbuf *m)
{
	struct port_info *pi = (void *)ifp->if_softc;
	struct adapter *sc = pi->adapter;
	struct sge_eq *eq = &txq->eq;
	struct buf_ring *br = eq->br;
	struct mbuf *next;
	int rc, coalescing;
	struct txpkts txpkts;
	struct sgl sgl;

	TXQ_LOCK_ASSERT_OWNED(txq);
	KASSERT(m, ("%s: called with nothing to do.", __func__));

	txpkts.npkt = 0;/* indicates there's nothing in txpkts */
	coalescing = 0;

	prefetch(&eq->sdesc[eq->pidx]);
	prefetch(&eq->desc[eq->pidx]);
	prefetch(&eq->maps[eq->map_pidx]);

	if (eq->avail < 8)
		reclaim_tx_descs(eq, 1, 8);

	for (; m; m = next ? next : drbr_dequeue(ifp, br)) {

		if (eq->avail < 8)
			break;

		next = m->m_nextpkt;
		m->m_nextpkt = NULL;

		if (next || buf_ring_peek(br))
			coalescing = 1;

		rc = get_pkt_sgl(txq, &m, &sgl, coalescing);
		if (rc != 0) {
			if (rc == ENOMEM) {

				/* Short of resources, suspend tx */

				m->m_nextpkt = next;
				break;
			}

			/*
			 * Unrecoverable error for this packet, throw it away
			 * and move on to the next.  get_pkt_sgl may already
			 * have freed m (it will be NULL in that case and the
			 * m_freem here is still safe).
			 */

			m_freem(m);
			continue;
		}

		if (coalescing &&
		    add_to_txpkts(pi, txq, &txpkts, m, &sgl) == 0) {

			/* Successfully absorbed into txpkts */

			write_ulp_cpl_sgl(pi, txq, &txpkts, m, &sgl);
			goto doorbell;
		}

		/*
		 * We weren't coalescing to begin with, or current frame could
		 * not be coalesced (add_to_txpkts flushes txpkts if a frame
		 * given to it can't be coalesced).  Either way there should be
		 * nothing in txpkts.
		 */
		KASSERT(txpkts.npkt == 0,
		    ("%s: txpkts not empty: %d", __func__, txpkts.npkt));

		/* We're sending out individual packets now */
		coalescing = 0;

		if (eq->avail < 8)
			reclaim_tx_descs(eq, 1, 8);
		rc = write_txpkt_wr(pi, txq, m, &sgl);
		if (rc != 0) {

			/* Short of hardware descriptors, suspend tx */

			/*
			 * This is an unlikely but expensive failure.  We've
			 * done all the hard work (DMA mappings etc.) and now we
			 * can't send out the packet.  What's worse, we have to
			 * spend even more time freeing up everything in sgl.
			 */
			txq->no_desc++;
			free_pkt_sgl(txq, &sgl);

			m->m_nextpkt = next;
			break;
		}

		ETHER_BPF_MTAP(ifp, m);
		if (sgl.nsegs == 0)
			m_freem(m);

doorbell:
		/* Fewer and fewer doorbells as the queue fills up */
		if (eq->pending >= (1 << (fls(eq->qsize - eq->avail) / 2)))
		    ring_tx_db(sc, eq);
		reclaim_tx_descs(eq, 16, 32);
	}

	if (txpkts.npkt > 0)
		write_txpkts_wr(txq, &txpkts);

	/*
	 * m not NULL means there was an error but we haven't thrown it away.
	 * This can happen when we're short of tx descriptors (no_desc) or maybe
	 * even DMA maps (no_dmamap).  Either way, a credit flush and reclaim
	 * will get things going again.
	 *
	 * If eq->avail is already 0 we know a credit flush was requested in the
	 * WR that reduced it to 0 so we don't need another flush (we don't have
	 * any descriptor for a flush WR anyway, duh).
	 */
	if (m && eq->avail > 0)
		write_eqflush_wr(eq);
	txq->m = m;

	if (eq->pending)
		ring_tx_db(sc, eq);

	reclaim_tx_descs(eq, 16, eq->qsize);

	return (0);
}

void
t4_update_fl_bufsize(struct ifnet *ifp)
{
	struct port_info *pi = ifp->if_softc;
	struct sge_rxq *rxq;
	struct sge_fl *fl;
	int i;

	for_each_rxq(pi, i, rxq) {
		fl = &rxq->fl;

		FL_LOCK(fl);
		set_fl_tag_idx(fl, ifp->if_mtu);
		FL_UNLOCK(fl);
	}
}

/*
 * A non-NULL handler indicates this iq will not receive direct interrupts, the
 * handler will be invoked by a forwarded interrupt queue.
 */
static inline void
init_iq(struct sge_iq *iq, struct adapter *sc, int tmr_idx, int pktc_idx,
    int qsize, int esize, iq_intr_handler_t *handler, char *name)
{
	KASSERT(tmr_idx >= 0 && tmr_idx < SGE_NTIMERS,
	    ("%s: bad tmr_idx %d", __func__, tmr_idx));
	KASSERT(pktc_idx < SGE_NCOUNTERS,	/* -ve is ok, means don't use */
	    ("%s: bad pktc_idx %d", __func__, pktc_idx));

	iq->flags = 0;
	iq->adapter = sc;
	iq->intr_params = V_QINTR_TIMER_IDX(tmr_idx) |
	    V_QINTR_CNT_EN(pktc_idx >= 0);
	iq->intr_pktc_idx = pktc_idx;
	iq->qsize = roundup(qsize, 16);		/* See FW_IQ_CMD/iqsize */
	iq->esize = max(esize, 16);		/* See FW_IQ_CMD/iqesize */
	iq->handler = handler;
	strlcpy(iq->lockname, name, sizeof(iq->lockname));
}

static inline void
init_fl(struct sge_fl *fl, int qsize, char *name)
{
	fl->qsize = qsize;
	strlcpy(fl->lockname, name, sizeof(fl->lockname));
}

static inline void
init_txq(struct sge_txq *txq, int qsize, char *name)
{
	txq->eq.qsize = qsize;
	strlcpy(txq->eq.lockname, name, sizeof(txq->eq.lockname));
}

static int
alloc_ring(struct adapter *sc, size_t len, bus_dma_tag_t *tag,
    bus_dmamap_t *map, bus_addr_t *pa, void **va)
{
	int rc;

	rc = bus_dma_tag_create(sc->dmat, 512, 0, BUS_SPACE_MAXADDR,
	    BUS_SPACE_MAXADDR, NULL, NULL, len, 1, len, 0, NULL, NULL, tag);
	if (rc != 0) {
		device_printf(sc->dev, "cannot allocate DMA tag: %d\n", rc);
		goto done;
	}

	rc = bus_dmamem_alloc(*tag, va,
	    BUS_DMA_WAITOK | BUS_DMA_COHERENT | BUS_DMA_ZERO, map);
	if (rc != 0) {
		device_printf(sc->dev, "cannot allocate DMA memory: %d\n", rc);
		goto done;
	}

	rc = bus_dmamap_load(*tag, *map, *va, len, oneseg_dma_callback, pa, 0);
	if (rc != 0) {
		device_printf(sc->dev, "cannot load DMA map: %d\n", rc);
		goto done;
	}
done:
	if (rc)
		free_ring(sc, *tag, *map, *pa, *va);

	return (rc);
}

static int
free_ring(struct adapter *sc, bus_dma_tag_t tag, bus_dmamap_t map,
    bus_addr_t pa, void *va)
{
	if (pa)
		bus_dmamap_unload(tag, map);
	if (va)
		bus_dmamem_free(tag, va, map);
	if (tag)
		bus_dma_tag_destroy(tag);

	return (0);
}

/*
 * Allocates the ring for an ingress queue and an optional freelist.  If the
 * freelist is specified it will be allocated and then associated with the
 * ingress queue.
 *
 * Returns errno on failure.  Resources allocated up to that point may still be
 * allocated.  Caller is responsible for cleanup in case this function fails.
 *
 * If the ingress queue will take interrupts directly (iq->handler == NULL) then
 * the intr_idx specifies the vector, starting from 0.  Otherwise it specifies
 * the index of the queue to which its interrupts will be forwarded.
 */
static int
alloc_iq_fl(struct port_info *pi, struct sge_iq *iq, struct sge_fl *fl,
    int intr_idx)
{
	int rc, i, cntxt_id;
	size_t len;
	struct fw_iq_cmd c;
	struct adapter *sc = iq->adapter;
	__be32 v = 0;

	/* The adapter queues are nominally allocated in port[0]'s name */
	if (pi == NULL)
		pi = sc->port[0];

	mtx_init(&iq->iq_lock, iq->lockname, NULL, MTX_DEF);

	len = iq->qsize * iq->esize;
	rc = alloc_ring(sc, len, &iq->desc_tag, &iq->desc_map, &iq->ba,
	    (void **)&iq->desc);
	if (rc != 0)
		return (rc);

	bzero(&c, sizeof(c));
	c.op_to_vfn = htobe32(V_FW_CMD_OP(FW_IQ_CMD) | F_FW_CMD_REQUEST |
	    F_FW_CMD_WRITE | F_FW_CMD_EXEC | V_FW_IQ_CMD_PFN(sc->pf) |
	    V_FW_IQ_CMD_VFN(0));

	c.alloc_to_len16 = htobe32(F_FW_IQ_CMD_ALLOC | F_FW_IQ_CMD_IQSTART |
	    FW_LEN16(c));

	/* Special handling for firmware event queue */
	if (iq == &sc->sge.fwq)
		v |= F_FW_IQ_CMD_IQASYNCH;

	if (iq->handler) {
		KASSERT(intr_idx < NFIQ(sc),
		    ("%s: invalid indirect intr_idx %d", __func__, intr_idx));
		v |= F_FW_IQ_CMD_IQANDST;
		v |= V_FW_IQ_CMD_IQANDSTINDEX(sc->sge.fiq[intr_idx].abs_id);
	} else {
		KASSERT(intr_idx < sc->intr_count,
		    ("%s: invalid direct intr_idx %d", __func__, intr_idx));
		v |= V_FW_IQ_CMD_IQANDSTINDEX(intr_idx);
	}

	c.type_to_iqandstindex = htobe32(v |
	    V_FW_IQ_CMD_TYPE(FW_IQ_TYPE_FL_INT_CAP) |
	    V_FW_IQ_CMD_VIID(pi->viid) |
	    V_FW_IQ_CMD_IQANUD(X_UPDATEDELIVERY_INTERRUPT));
	c.iqdroprss_to_iqesize = htobe16(V_FW_IQ_CMD_IQPCIECH(pi->tx_chan) |
	    F_FW_IQ_CMD_IQGTSMODE |
	    V_FW_IQ_CMD_IQINTCNTTHRESH(iq->intr_pktc_idx) |
	    V_FW_IQ_CMD_IQESIZE(ilog2(iq->esize) - 4));
	c.iqsize = htobe16(iq->qsize);
	c.iqaddr = htobe64(iq->ba);

	if (fl) {
		mtx_init(&fl->fl_lock, fl->lockname, NULL, MTX_DEF);

		for (i = 0; i < FL_BUF_SIZES; i++) {

			/*
			 * A freelist buffer must be 16 byte aligned as the SGE
			 * uses the low 4 bits of the bus addr to figure out the
			 * buffer size.
			 */
			rc = bus_dma_tag_create(sc->dmat, 16, 0,
			    BUS_SPACE_MAXADDR, BUS_SPACE_MAXADDR, NULL, NULL,
			    FL_BUF_SIZE(i), 1, FL_BUF_SIZE(i), BUS_DMA_ALLOCNOW,
			    NULL, NULL, &fl->tag[i]);
			if (rc != 0) {
				device_printf(sc->dev,
				    "failed to create fl DMA tag[%d]: %d\n",
				    i, rc);
				return (rc);
			}
		}
		len = fl->qsize * RX_FL_ESIZE;
		rc = alloc_ring(sc, len, &fl->desc_tag, &fl->desc_map,
		    &fl->ba, (void **)&fl->desc);
		if (rc)
			return (rc);

		/* Allocate space for one software descriptor per buffer. */
		fl->cap = (fl->qsize - SPG_LEN / RX_FL_ESIZE) * 8;
		FL_LOCK(fl);
		set_fl_tag_idx(fl, pi->ifp->if_mtu);
		rc = alloc_fl_sdesc(fl);
		FL_UNLOCK(fl);
		if (rc != 0) {
			device_printf(sc->dev,
			    "failed to setup fl software descriptors: %d\n",
			    rc);
			return (rc);
		}
		fl->needed = fl->cap - 1; /* one less to avoid cidx = pidx */

		c.iqns_to_fl0congen =
		    htobe32(V_FW_IQ_CMD_FL0HOSTFCMODE(X_HOSTFCMODE_NONE));
		c.fl0dcaen_to_fl0cidxfthresh =
		    htobe16(V_FW_IQ_CMD_FL0FBMIN(X_FETCHBURSTMIN_64B) |
			V_FW_IQ_CMD_FL0FBMAX(X_FETCHBURSTMAX_512B));
		c.fl0size = htobe16(fl->qsize);
		c.fl0addr = htobe64(fl->ba);
	}

	rc = -t4_wr_mbox(sc, sc->mbox, &c, sizeof(c), &c);
	if (rc != 0) {
		device_printf(sc->dev,
		    "failed to create ingress queue: %d\n", rc);
		return (rc);
	}

	iq->cdesc = iq->desc;
	iq->cidx = 0;
	iq->gen = 1;
	iq->intr_next = iq->intr_params;
	iq->cntxt_id = be16toh(c.iqid);
	iq->abs_id = be16toh(c.physiqid);
	iq->flags |= (IQ_ALLOCATED | IQ_STARTED);

	cntxt_id = iq->cntxt_id - sc->sge.iq_start;
	KASSERT(cntxt_id < sc->sge.niq,
	    ("%s: iq->cntxt_id (%d) more than the max (%d)", __func__,
	    cntxt_id, sc->sge.niq - 1));
	sc->sge.iqmap[cntxt_id] = iq;

	if (fl) {
		fl->cntxt_id = be16toh(c.fl0id);
		fl->pidx = fl->cidx = 0;

		cntxt_id = iq->cntxt_id - sc->sge.eq_start;
		KASSERT(cntxt_id < sc->sge.neq,
		    ("%s: fl->cntxt_id (%d) more than the max (%d)", __func__,
		    cntxt_id, sc->sge.neq - 1));
		sc->sge.eqmap[cntxt_id] = (void *)fl;

		FL_LOCK(fl);
		refill_fl(fl, -1);
		if (fl->pending >= 8)
			ring_fl_db(sc, fl);
		FL_UNLOCK(fl);
	}

	/* Enable IQ interrupts */
	t4_write_reg(sc, MYPF_REG(A_SGE_PF_GTS), V_SEINTARM(iq->intr_params) |
	    V_INGRESSQID(iq->cntxt_id));

	return (0);
}

/*
 * This can be called with the iq/fl in any state - fully allocated and
 * functional, partially allocated, even all-zeroed out.
 */
static int
free_iq_fl(struct port_info *pi, struct sge_iq *iq, struct sge_fl *fl)
{
	int i, rc;
	struct adapter *sc = iq->adapter;
	device_t dev;

	if (sc == NULL)
		return (0);	/* nothing to do */

	dev = pi ? pi->dev : sc->dev;

	if (iq->flags & IQ_STARTED) {
		rc = -t4_iq_start_stop(sc, sc->mbox, 0, sc->pf, 0,
		    iq->cntxt_id, fl ? fl->cntxt_id : 0xffff, 0xffff);
		if (rc != 0) {
			device_printf(dev,
			    "failed to stop queue %p: %d\n", iq, rc);
			return (rc);
		}
		iq->flags &= ~IQ_STARTED;
	}

	if (iq->flags & IQ_ALLOCATED) {

		rc = -t4_iq_free(sc, sc->mbox, sc->pf, 0,
		    FW_IQ_TYPE_FL_INT_CAP, iq->cntxt_id,
		    fl ? fl->cntxt_id : 0xffff, 0xffff);
		if (rc != 0) {
			device_printf(dev,
			    "failed to free queue %p: %d\n", iq, rc);
			return (rc);
		}
		iq->flags &= ~IQ_ALLOCATED;
	}

	free_ring(sc, iq->desc_tag, iq->desc_map, iq->ba, iq->desc);

	if (mtx_initialized(&iq->iq_lock))
		mtx_destroy(&iq->iq_lock);

	bzero(iq, sizeof(*iq));

	if (fl) {
		free_ring(sc, fl->desc_tag, fl->desc_map, fl->ba,
		    fl->desc);

		if (fl->sdesc) {
			FL_LOCK(fl);
			free_fl_sdesc(fl);
			FL_UNLOCK(fl);
		}

		if (mtx_initialized(&fl->fl_lock))
			mtx_destroy(&fl->fl_lock);

		for (i = 0; i < FL_BUF_SIZES; i++) {
			if (fl->tag[i])
				bus_dma_tag_destroy(fl->tag[i]);
		}

		bzero(fl, sizeof(*fl));
	}

	return (0);
}

static int
alloc_iq(struct sge_iq *iq, int intr_idx)
{
	return alloc_iq_fl(NULL, iq, NULL, intr_idx);
}

static int
free_iq(struct sge_iq *iq)
{
	return free_iq_fl(NULL, iq, NULL);
}

static int
alloc_rxq(struct port_info *pi, struct sge_rxq *rxq, int intr_idx, int idx)
{
	int rc;
	struct sysctl_oid *oid;
	struct sysctl_oid_list *children;
	char name[16];

	rc = alloc_iq_fl(pi, &rxq->iq, &rxq->fl, intr_idx);
	if (rc != 0)
		return (rc);

#ifdef INET
	rc = tcp_lro_init(&rxq->lro);
	if (rc != 0)
		return (rc);
	rxq->lro.ifp = pi->ifp; /* also indicates LRO init'ed */

	if (pi->ifp->if_capenable & IFCAP_LRO)
		rxq->flags |= RXQ_LRO_ENABLED;
#endif
	rxq->port = pi;

	children = SYSCTL_CHILDREN(pi->oid_rxq);

	snprintf(name, sizeof(name), "%d", idx);
	oid = SYSCTL_ADD_NODE(&pi->ctx, children, OID_AUTO, name, CTLFLAG_RD,
	    NULL, "rx queue");
	children = SYSCTL_CHILDREN(oid);

	SYSCTL_ADD_INT(&pi->ctx, children, OID_AUTO, "lro_queued", CTLFLAG_RD,
	    &rxq->lro.lro_queued, 0, NULL);
	SYSCTL_ADD_INT(&pi->ctx, children, OID_AUTO, "lro_flushed", CTLFLAG_RD,
	    &rxq->lro.lro_flushed, 0, NULL);
	SYSCTL_ADD_UQUAD(&pi->ctx, children, OID_AUTO, "rxcsum", CTLFLAG_RD,
	    &rxq->rxcsum, "# of times hardware assisted with checksum");
	SYSCTL_ADD_UQUAD(&pi->ctx, children, OID_AUTO, "vlan_extraction",
	    CTLFLAG_RD, &rxq->vlan_extraction,
	    "# of times hardware extracted 802.1Q tag");

	return (rc);
}

static int
free_rxq(struct port_info *pi, struct sge_rxq *rxq)
{
	int rc;

#ifdef INET
	if (rxq->lro.ifp) {
		tcp_lro_free(&rxq->lro);
		rxq->lro.ifp = NULL;
	}
#endif

	rc = free_iq_fl(pi, &rxq->iq, &rxq->fl);
	if (rc == 0)
		bzero(rxq, sizeof(*rxq));

	return (rc);
}

static int
alloc_txq(struct port_info *pi, struct sge_txq *txq, int idx)
{
	int rc, cntxt_id;
	size_t len;
	struct adapter *sc = pi->adapter;
	struct fw_eq_eth_cmd c;
	struct sge_eq *eq = &txq->eq;
	char name[16];
	struct sysctl_oid *oid;
	struct sysctl_oid_list *children;

	txq->port = pi;
	TASK_INIT(&txq->resume_tx, 0, cxgbe_txq_start, txq);

	mtx_init(&eq->eq_lock, eq->lockname, NULL, MTX_DEF);

	len = eq->qsize * TX_EQ_ESIZE;
	rc = alloc_ring(sc, len, &eq->desc_tag, &eq->desc_map,
	    &eq->ba, (void **)&eq->desc);
	if (rc)
		return (rc);

	eq->cap = eq->qsize - SPG_LEN / TX_EQ_ESIZE;
	eq->spg = (void *)&eq->desc[eq->cap];
	eq->avail = eq->cap - 1;	/* one less to avoid cidx = pidx */
	eq->sdesc = malloc(eq->cap * sizeof(struct tx_sdesc), M_CXGBE,
	    M_ZERO | M_WAITOK);
	eq->br = buf_ring_alloc(eq->qsize, M_CXGBE, M_WAITOK, &eq->eq_lock);

	rc = bus_dma_tag_create(sc->dmat, 1, 0, BUS_SPACE_MAXADDR,
	    BUS_SPACE_MAXADDR, NULL, NULL, 64 * 1024, TX_SGL_SEGS,
	    BUS_SPACE_MAXSIZE, BUS_DMA_ALLOCNOW, NULL, NULL, &eq->tx_tag);
	if (rc != 0) {
		device_printf(sc->dev,
		    "failed to create tx DMA tag: %d\n", rc);
		return (rc);
	}

	rc = alloc_eq_maps(eq);
	if (rc != 0) {
		device_printf(sc->dev, "failed to setup tx DMA maps: %d\n", rc);
		return (rc);
	}

	bzero(&c, sizeof(c));

	c.op_to_vfn = htobe32(V_FW_CMD_OP(FW_EQ_ETH_CMD) | F_FW_CMD_REQUEST |
	    F_FW_CMD_WRITE | F_FW_CMD_EXEC | V_FW_EQ_ETH_CMD_PFN(sc->pf) |
	    V_FW_EQ_ETH_CMD_VFN(0));
	c.alloc_to_len16 = htobe32(F_FW_EQ_ETH_CMD_ALLOC |
	    F_FW_EQ_ETH_CMD_EQSTART | FW_LEN16(c));
	c.viid_pkd = htobe32(V_FW_EQ_ETH_CMD_VIID(pi->viid));
	c.fetchszm_to_iqid =
	    htobe32(V_FW_EQ_ETH_CMD_HOSTFCMODE(X_HOSTFCMODE_STATUS_PAGE) |
		V_FW_EQ_ETH_CMD_PCIECHN(pi->tx_chan) |
		V_FW_EQ_ETH_CMD_IQID(sc->sge.rxq[pi->first_rxq].iq.cntxt_id));
	c.dcaen_to_eqsize = htobe32(V_FW_EQ_ETH_CMD_FBMIN(X_FETCHBURSTMIN_64B) |
		      V_FW_EQ_ETH_CMD_FBMAX(X_FETCHBURSTMAX_512B) |
		      V_FW_EQ_ETH_CMD_CIDXFTHRESH(X_CIDXFLUSHTHRESH_32) |
		      V_FW_EQ_ETH_CMD_EQSIZE(eq->qsize));
	c.eqaddr = htobe64(eq->ba);

	rc = -t4_wr_mbox(sc, sc->mbox, &c, sizeof(c), &c);
	if (rc != 0) {
		device_printf(pi->dev,
		    "failed to create egress queue: %d\n", rc);
		return (rc);
	}

	eq->pidx = eq->cidx = 0;
	eq->cntxt_id = G_FW_EQ_ETH_CMD_EQID(be32toh(c.eqid_pkd));
	eq->flags |= (EQ_ALLOCATED | EQ_STARTED);

	cntxt_id = eq->cntxt_id - sc->sge.eq_start;
	KASSERT(cntxt_id < sc->sge.neq,
	    ("%s: eq->cntxt_id (%d) more than the max (%d)", __func__,
	    cntxt_id, sc->sge.neq - 1));
	sc->sge.eqmap[cntxt_id] = eq;

	children = SYSCTL_CHILDREN(pi->oid_txq);

	snprintf(name, sizeof(name), "%d", idx);
	oid = SYSCTL_ADD_NODE(&pi->ctx, children, OID_AUTO, name, CTLFLAG_RD,
	    NULL, "tx queue");
	children = SYSCTL_CHILDREN(oid);

	SYSCTL_ADD_UQUAD(&pi->ctx, children, OID_AUTO, "txcsum", CTLFLAG_RD,
	    &txq->txcsum, "# of times hardware assisted with checksum");
	SYSCTL_ADD_UQUAD(&pi->ctx, children, OID_AUTO, "vlan_insertion",
	    CTLFLAG_RD, &txq->vlan_insertion,
	    "# of times hardware inserted 802.1Q tag");
	SYSCTL_ADD_UQUAD(&pi->ctx, children, OID_AUTO, "tso_wrs", CTLFLAG_RD,
	    &txq->tso_wrs, "# of IPv4 TSO work requests");
	SYSCTL_ADD_UQUAD(&pi->ctx, children, OID_AUTO, "imm_wrs", CTLFLAG_RD,
	    &txq->imm_wrs, "# of work requests with immediate data");
	SYSCTL_ADD_UQUAD(&pi->ctx, children, OID_AUTO, "sgl_wrs", CTLFLAG_RD,
	    &txq->sgl_wrs, "# of work requests with direct SGL");
	SYSCTL_ADD_UQUAD(&pi->ctx, children, OID_AUTO, "txpkt_wrs", CTLFLAG_RD,
	    &txq->txpkt_wrs, "# of txpkt work requests (one pkt/WR)");
	SYSCTL_ADD_UQUAD(&pi->ctx, children, OID_AUTO, "txpkts_wrs", CTLFLAG_RD,
	    &txq->txpkts_wrs, "# of txpkts work requests (multiple pkts/WR)");
	SYSCTL_ADD_UQUAD(&pi->ctx, children, OID_AUTO, "txpkts_pkts", CTLFLAG_RD,
	    &txq->txpkts_pkts, "# of frames tx'd using txpkts work requests");

	SYSCTL_ADD_UINT(&pi->ctx, children, OID_AUTO, "no_dmamap", CTLFLAG_RD,
	    &txq->no_dmamap, 0, "# of times txq ran out of DMA maps");
	SYSCTL_ADD_UINT(&pi->ctx, children, OID_AUTO, "no_desc", CTLFLAG_RD,
	    &txq->no_desc, 0, "# of times txq ran out of hardware descriptors");
	SYSCTL_ADD_UINT(&pi->ctx, children, OID_AUTO, "egr_update", CTLFLAG_RD,
	    &txq->egr_update, 0, "egress update notifications from the SGE");

	return (rc);
}

static int
free_txq(struct port_info *pi, struct sge_txq *txq)
{
	int rc;
	struct adapter *sc = pi->adapter;
	struct sge_eq *eq = &txq->eq;

	if (eq->flags & (EQ_ALLOCATED | EQ_STARTED)) {
		rc = -t4_eth_eq_free(sc, sc->mbox, sc->pf, 0, eq->cntxt_id);
		if (rc != 0) {
			device_printf(pi->dev,
			    "failed to free egress queue %p: %d\n", eq, rc);
			return (rc);
		}
		eq->flags &= ~(EQ_ALLOCATED | EQ_STARTED);
	}

	free_ring(sc, eq->desc_tag, eq->desc_map, eq->ba, eq->desc);

	free(eq->sdesc, M_CXGBE);

	if (eq->maps)
		free_eq_maps(eq);

	buf_ring_free(eq->br, M_CXGBE);

	if (eq->tx_tag)
		bus_dma_tag_destroy(eq->tx_tag);

	if (mtx_initialized(&eq->eq_lock))
		mtx_destroy(&eq->eq_lock);

	bzero(txq, sizeof(*txq));
	return (0);
}

static void
oneseg_dma_callback(void *arg, bus_dma_segment_t *segs, int nseg, int error)
{
	bus_addr_t *ba = arg;

	KASSERT(nseg == 1,
	    ("%s meant for single segment mappings only.", __func__));

	*ba = error ? 0 : segs->ds_addr;
}

static inline bool
is_new_response(const struct sge_iq *iq, struct rsp_ctrl **ctrl)
{
	*ctrl = (void *)((uintptr_t)iq->cdesc +
	    (iq->esize - sizeof(struct rsp_ctrl)));

	return (((*ctrl)->u.type_gen >> S_RSPD_GEN) == iq->gen);
}

static inline void
iq_next(struct sge_iq *iq)
{
	iq->cdesc = (void *) ((uintptr_t)iq->cdesc + iq->esize);
	if (__predict_false(++iq->cidx == iq->qsize - 1)) {
		iq->cidx = 0;
		iq->gen ^= 1;
		iq->cdesc = iq->desc;
	}
}

static inline void
ring_fl_db(struct adapter *sc, struct sge_fl *fl)
{
	int ndesc = fl->pending / 8;

	/* Caller responsible for ensuring there's something useful to do */
	KASSERT(ndesc > 0, ("%s called with no useful work to do.", __func__));

	wmb();

	t4_write_reg(sc, MYPF_REG(A_SGE_PF_KDOORBELL), F_DBPRIO |
	    V_QID(fl->cntxt_id) | V_PIDX(ndesc));

	fl->pending &= 7;
}

static void
refill_fl(struct sge_fl *fl, int nbufs)
{
	__be64 *d = &fl->desc[fl->pidx];
	struct fl_sdesc *sd = &fl->sdesc[fl->pidx];
	bus_dma_tag_t tag;
	bus_addr_t pa;
	caddr_t cl;
	int rc;

	FL_LOCK_ASSERT_OWNED(fl);

	if (nbufs < 0 || nbufs > fl->needed)
		nbufs = fl->needed;

	while (nbufs--) {

		if (sd->cl != NULL) {

			/*
			 * This happens when a frame small enough to fit
			 * entirely in an mbuf was received in cl last time.
			 * We'd held on to cl and can reuse it now.  Note that
			 * we reuse a cluster of the old size if fl->tag_idx is
			 * no longer the same as sd->tag_idx.
			 */

			KASSERT(*d == sd->ba_tag,
			    ("%s: recyling problem at pidx %d",
			    __func__, fl->pidx));

			d++;
			goto recycled;
		}


		if (fl->tag_idx != sd->tag_idx) {
			bus_dmamap_t map;
			bus_dma_tag_t newtag = fl->tag[fl->tag_idx];
			bus_dma_tag_t oldtag = fl->tag[sd->tag_idx];

			/*
			 * An MTU change can get us here.  Discard the old map
			 * which was created with the old tag, but only if
			 * we're able to get a new one.
			 */
			rc = bus_dmamap_create(newtag, 0, &map);
			if (rc == 0) {
				bus_dmamap_destroy(oldtag, sd->map);
				sd->map = map;
				sd->tag_idx = fl->tag_idx;
			}
		}

		tag = fl->tag[sd->tag_idx];

		cl = m_cljget(NULL, M_NOWAIT, FL_BUF_SIZE(sd->tag_idx));
		if (cl == NULL)
			break;

		rc = bus_dmamap_load(tag, sd->map, cl,
		    FL_BUF_SIZE(sd->tag_idx), oneseg_dma_callback,
		    &pa, 0);
		if (rc != 0 || pa == 0) {
			fl->dmamap_failed++;
			uma_zfree(FL_BUF_ZONE(sd->tag_idx), cl);
			break;
		}

		sd->cl = cl;
		*d++ = htobe64(pa | sd->tag_idx);

#ifdef INVARIANTS
		sd->ba_tag = htobe64(pa | sd->tag_idx);
#endif

recycled:	fl->pending++;
		fl->needed--;
		sd++;
		if (++fl->pidx == fl->cap) {
			fl->pidx = 0;
			sd = fl->sdesc;
			d = fl->desc;
		}

		/* No harm if gethdr fails, we'll retry after rx */
		if (sd->m == NULL)
			sd->m = m_gethdr(M_NOWAIT, MT_NOINIT);
	}
}

static int
alloc_fl_sdesc(struct sge_fl *fl)
{
	struct fl_sdesc *sd;
	bus_dma_tag_t tag;
	int i, rc;

	FL_LOCK_ASSERT_OWNED(fl);

	fl->sdesc = malloc(fl->cap * sizeof(struct fl_sdesc), M_CXGBE,
	    M_ZERO | M_WAITOK);

	tag = fl->tag[fl->tag_idx];
	sd = fl->sdesc;
	for (i = 0; i < fl->cap; i++, sd++) {

		sd->tag_idx = fl->tag_idx;
		rc = bus_dmamap_create(tag, 0, &sd->map);
		if (rc != 0)
			goto failed;

		/* Doesn't matter if this succeeds or not */
		sd->m = m_gethdr(M_NOWAIT, MT_NOINIT);
	}

	return (0);
failed:
	while (--i >= 0) {
		sd--;
		bus_dmamap_destroy(tag, sd->map);
		if (sd->m) {
			m_init(sd->m, zone_mbuf, MLEN, M_NOWAIT, MT_DATA, 0);
			m_free(sd->m);
			sd->m = NULL;
		}
	}
	KASSERT(sd == fl->sdesc, ("%s: EDOOFUS", __func__));

	free(fl->sdesc, M_CXGBE);
	fl->sdesc = NULL;

	return (rc);
}

static void
free_fl_sdesc(struct sge_fl *fl)
{
	struct fl_sdesc *sd;
	int i;

	FL_LOCK_ASSERT_OWNED(fl);

	sd = fl->sdesc;
	for (i = 0; i < fl->cap; i++, sd++) {

		if (sd->m) {
			m_init(sd->m, zone_mbuf, MLEN, M_NOWAIT, MT_DATA, 0);
			m_free(sd->m);
			sd->m = NULL;
		}

		if (sd->cl) {
			bus_dmamap_unload(fl->tag[sd->tag_idx], sd->map);
			uma_zfree(FL_BUF_ZONE(sd->tag_idx), sd->cl);
			sd->cl = NULL;
		}

		bus_dmamap_destroy(fl->tag[sd->tag_idx], sd->map);
	}

	free(fl->sdesc, M_CXGBE);
	fl->sdesc = NULL;
}

static int
alloc_eq_maps(struct sge_eq *eq)
{
	struct tx_map *txm;
	int i, rc, count;

	/*
	 * We can stuff ~10 frames in an 8-descriptor txpkts WR (8 is the SGE
	 * limit for any WR).  txq->no_dmamap events shouldn't occur if maps is
	 * sized for the worst case.
	 */
	count = eq->qsize * 10 / 8;
	eq->map_total = eq->map_avail = count;
	eq->map_cidx = eq->map_pidx = 0;

	eq->maps = malloc(count * sizeof(struct tx_map), M_CXGBE,
	    M_ZERO | M_WAITOK);

	txm = eq->maps;
	for (i = 0; i < count; i++, txm++) {
		rc = bus_dmamap_create(eq->tx_tag, 0, &txm->map);
		if (rc != 0)
			goto failed;
	}

	return (0);
failed:
	while (--i >= 0) {
		txm--;
		bus_dmamap_destroy(eq->tx_tag, txm->map);
	}
	KASSERT(txm == eq->maps, ("%s: EDOOFUS", __func__));

	free(eq->maps, M_CXGBE);
	eq->maps = NULL;

	return (rc);
}

static void
free_eq_maps(struct sge_eq *eq)
{
	struct tx_map *txm;
	int i;

	txm = eq->maps;
	for (i = 0; i < eq->map_total; i++, txm++) {

		if (txm->m) {
			bus_dmamap_unload(eq->tx_tag, txm->map);
			m_freem(txm->m);
			txm->m = NULL;
		}

		bus_dmamap_destroy(eq->tx_tag, txm->map);
	}

	free(eq->maps, M_CXGBE);
	eq->maps = NULL;
}

/*
 * We'll do immediate data tx for non-TSO, but only when not coalescing.  We're
 * willing to use upto 2 hardware descriptors which means a maximum of 96 bytes
 * of immediate data.
 */
#define IMM_LEN ( \
      2 * TX_EQ_ESIZE \
    - sizeof(struct fw_eth_tx_pkt_wr) \
    - sizeof(struct cpl_tx_pkt_core))

/*
 * Returns non-zero on failure, no need to cleanup anything in that case.
 *
 * Note 1: We always try to defrag the mbuf if required and return EFBIG only
 * if the resulting chain still won't fit in a tx descriptor.
 *
 * Note 2: We'll pullup the mbuf chain if TSO is requested and the first mbuf
 * does not have the TCP header in it.
 */
static int
get_pkt_sgl(struct sge_txq *txq, struct mbuf **fp, struct sgl *sgl,
    int sgl_only)
{
	struct mbuf *m = *fp;
	struct sge_eq *eq = &txq->eq;
	struct tx_map *txm;
	int rc, defragged = 0, n;

	TXQ_LOCK_ASSERT_OWNED(txq);

	if (m->m_pkthdr.tso_segsz)
		sgl_only = 1;	/* Do not allow immediate data with LSO */

start:	sgl->nsegs = 0;

	if (m->m_pkthdr.len <= IMM_LEN && !sgl_only)
		return (0);	/* nsegs = 0 tells caller to use imm. tx */

	if (eq->map_avail == 0) {
		txq->no_dmamap++;
		return (ENOMEM);
	}
	txm = &eq->maps[eq->map_pidx];

	if (m->m_pkthdr.tso_segsz && m->m_len < 50) {
		*fp = m_pullup(m, 50);
		m = *fp;
		if (m == NULL)
			return (ENOBUFS);
	}

	rc = bus_dmamap_load_mbuf_sg(eq->tx_tag, txm->map, m, sgl->seg,
	    &sgl->nsegs, BUS_DMA_NOWAIT);
	if (rc == EFBIG && defragged == 0) {
		m = m_defrag(m, M_DONTWAIT);
		if (m == NULL)
			return (EFBIG);

		defragged = 1;
		*fp = m;
		goto start;
	}
	if (rc != 0)
		return (rc);

	txm->m = m;
	eq->map_avail--;
	if (++eq->map_pidx == eq->map_total)
		eq->map_pidx = 0;

	KASSERT(sgl->nsegs > 0 && sgl->nsegs <= TX_SGL_SEGS,
	    ("%s: bad DMA mapping (%d segments)", __func__, sgl->nsegs));

	/*
	 * Store the # of flits required to hold this frame's SGL in nflits.  An
	 * SGL has a (ULPTX header + len0, addr0) tuple optionally followed by
	 * multiple (len0 + len1, addr0, addr1) tuples.  If addr1 is not used
	 * then len1 must be set to 0.
	 */
	n = sgl->nsegs - 1;
	sgl->nflits = (3 * n) / 2 + (n & 1) + 2;

	return (0);
}


/*
 * Releases all the txq resources used up in the specified sgl.
 */
static int
free_pkt_sgl(struct sge_txq *txq, struct sgl *sgl)
{
	struct sge_eq *eq = &txq->eq;
	struct tx_map *txm;

	TXQ_LOCK_ASSERT_OWNED(txq);

	if (sgl->nsegs == 0)
		return (0);	/* didn't use any map */

	/* 1 pkt uses exactly 1 map, back it out */

	eq->map_avail++;
	if (eq->map_pidx > 0)
		eq->map_pidx--;
	else
		eq->map_pidx = eq->map_total - 1;

	txm = &eq->maps[eq->map_pidx];
	bus_dmamap_unload(eq->tx_tag, txm->map);
	txm->m = NULL;

	return (0);
}

static int
write_txpkt_wr(struct port_info *pi, struct sge_txq *txq, struct mbuf *m,
    struct sgl *sgl)
{
	struct sge_eq *eq = &txq->eq;
	struct fw_eth_tx_pkt_wr *wr;
	struct cpl_tx_pkt_core *cpl;
	uint32_t ctrl;	/* used in many unrelated places */
	uint64_t ctrl1;
	int nflits, ndesc, pktlen;
	struct tx_sdesc *txsd;
	caddr_t dst;

	TXQ_LOCK_ASSERT_OWNED(txq);

	pktlen = m->m_pkthdr.len;

	/*
	 * Do we have enough flits to send this frame out?
	 */
	ctrl = sizeof(struct cpl_tx_pkt_core);
	if (m->m_pkthdr.tso_segsz) {
		nflits = TXPKT_LSO_WR_HDR;
		ctrl += sizeof(struct cpl_tx_pkt_lso);
	} else
		nflits = TXPKT_WR_HDR;
	if (sgl->nsegs > 0)
		nflits += sgl->nflits;
	else {
		nflits += howmany(pktlen, 8);
		ctrl += pktlen;
	}
	ndesc = howmany(nflits, 8);
	if (ndesc > eq->avail)
		return (ENOMEM);

	/* Firmware work request header */
	wr = (void *)&eq->desc[eq->pidx];
	wr->op_immdlen = htobe32(V_FW_WR_OP(FW_ETH_TX_PKT_WR) |
	    V_FW_WR_IMMDLEN(ctrl));
	ctrl = V_FW_WR_LEN16(howmany(nflits, 2));
	if (eq->avail == ndesc)
		ctrl |= F_FW_WR_EQUEQ | F_FW_WR_EQUIQ;
	wr->equiq_to_len16 = htobe32(ctrl);
	wr->r3 = 0;

	if (m->m_pkthdr.tso_segsz) {
		struct cpl_tx_pkt_lso *lso = (void *)(wr + 1);
		struct ether_header *eh;
		struct ip *ip;
		struct tcphdr *tcp;

		ctrl = V_LSO_OPCODE(CPL_TX_PKT_LSO) | F_LSO_FIRST_SLICE |
		    F_LSO_LAST_SLICE;

		eh = mtod(m, struct ether_header *);
		if (eh->ether_type == htons(ETHERTYPE_VLAN)) {
			ctrl |= V_LSO_ETHHDR_LEN(1);
			ip = (void *)((struct ether_vlan_header *)eh + 1);
		} else
			ip = (void *)(eh + 1);

		tcp = (void *)((uintptr_t)ip + ip->ip_hl * 4);
		ctrl |= V_LSO_IPHDR_LEN(ip->ip_hl) |
		    V_LSO_TCPHDR_LEN(tcp->th_off);

		lso->lso_ctrl = htobe32(ctrl);
		lso->ipid_ofst = htobe16(0);
		lso->mss = htobe16(m->m_pkthdr.tso_segsz);
		lso->seqno_offset = htobe32(0);
		lso->len = htobe32(pktlen);

		cpl = (void *)(lso + 1);

		txq->tso_wrs++;
	} else
		cpl = (void *)(wr + 1);

	/* Checksum offload */
	ctrl1 = 0;
	if (!(m->m_pkthdr.csum_flags & CSUM_IP))
		ctrl1 |= F_TXPKT_IPCSUM_DIS;
	if (!(m->m_pkthdr.csum_flags & (CSUM_TCP | CSUM_UDP)))
		ctrl1 |= F_TXPKT_L4CSUM_DIS;
	if (m->m_pkthdr.csum_flags & (CSUM_IP | CSUM_TCP | CSUM_UDP))
		txq->txcsum++;	/* some hardware assistance provided */

	/* VLAN tag insertion */
	if (m->m_flags & M_VLANTAG) {
		ctrl1 |= F_TXPKT_VLAN_VLD | V_TXPKT_VLAN(m->m_pkthdr.ether_vtag);
		txq->vlan_insertion++;
	}

	/* CPL header */
	cpl->ctrl0 = htobe32(V_TXPKT_OPCODE(CPL_TX_PKT) |
	    V_TXPKT_INTF(pi->tx_chan) | V_TXPKT_PF(pi->adapter->pf));
	cpl->pack = 0;
	cpl->len = htobe16(pktlen);
	cpl->ctrl1 = htobe64(ctrl1);

	/* Software descriptor */
	txsd = &eq->sdesc[eq->pidx];
	txsd->desc_used = ndesc;

	eq->pending += ndesc;
	eq->avail -= ndesc;
	eq->pidx += ndesc;
	if (eq->pidx >= eq->cap)
		eq->pidx -= eq->cap;

	/* SGL */
	dst = (void *)(cpl + 1);
	if (sgl->nsegs > 0) {
		txsd->map_used = 1;
		txq->sgl_wrs++;
		write_sgl_to_txd(eq, sgl, &dst);
	} else {
		txsd->map_used = 0;
		txq->imm_wrs++;
		for (; m; m = m->m_next) {
			copy_to_txd(eq, mtod(m, caddr_t), &dst, m->m_len);
#ifdef INVARIANTS
			pktlen -= m->m_len;
#endif
		}
#ifdef INVARIANTS
		KASSERT(pktlen == 0, ("%s: %d bytes left.", __func__, pktlen));
#endif

	}

	txq->txpkt_wrs++;
	return (0);
}

/*
 * Returns 0 to indicate that m has been accepted into a coalesced tx work
 * request.  It has either been folded into txpkts or txpkts was flushed and m
 * has started a new coalesced work request (as the first frame in a fresh
 * txpkts).
 *
 * Returns non-zero to indicate a failure - caller is responsible for
 * transmitting m, if there was anything in txpkts it has been flushed.
 */
static int
add_to_txpkts(struct port_info *pi, struct sge_txq *txq, struct txpkts *txpkts,
    struct mbuf *m, struct sgl *sgl)
{
	struct sge_eq *eq = &txq->eq;
	int can_coalesce;
	struct tx_sdesc *txsd;
	int flits;

	TXQ_LOCK_ASSERT_OWNED(txq);

	if (txpkts->npkt > 0) {
		flits = TXPKTS_PKT_HDR + sgl->nflits;
		can_coalesce = m->m_pkthdr.tso_segsz == 0 &&
		    txpkts->nflits + flits <= TX_WR_FLITS &&
		    txpkts->nflits + flits <= eq->avail * 8 &&
		    txpkts->plen + m->m_pkthdr.len < 65536;

		if (can_coalesce) {
			txpkts->npkt++;
			txpkts->nflits += flits;
			txpkts->plen += m->m_pkthdr.len;

			txsd = &eq->sdesc[eq->pidx];
			txsd->map_used++;

			return (0);
		}

		/*
		 * Couldn't coalesce m into txpkts.  The first order of business
		 * is to send txpkts on its way.  Then we'll revisit m.
		 */
		write_txpkts_wr(txq, txpkts);
	}

	/*
	 * Check if we can start a new coalesced tx work request with m as
	 * the first packet in it.
	 */

	KASSERT(txpkts->npkt == 0, ("%s: txpkts not empty", __func__));

	flits = TXPKTS_WR_HDR + sgl->nflits;
	can_coalesce = m->m_pkthdr.tso_segsz == 0 &&
	    flits <= eq->avail * 8 && flits <= TX_WR_FLITS;

	if (can_coalesce == 0)
		return (EINVAL);

	/*
	 * Start a fresh coalesced tx WR with m as the first frame in it.
	 */
	txpkts->npkt = 1;
	txpkts->nflits = flits;
	txpkts->flitp = &eq->desc[eq->pidx].flit[2];
	txpkts->plen = m->m_pkthdr.len;

	txsd = &eq->sdesc[eq->pidx];
	txsd->map_used = 1;

	return (0);
}

/*
 * Note that write_txpkts_wr can never run out of hardware descriptors (but
 * write_txpkt_wr can).  add_to_txpkts ensures that a frame is accepted for
 * coalescing only if sufficient hardware descriptors are available.
 */
static void
write_txpkts_wr(struct sge_txq *txq, struct txpkts *txpkts)
{
	struct sge_eq *eq = &txq->eq;
	struct fw_eth_tx_pkts_wr *wr;
	struct tx_sdesc *txsd;
	uint32_t ctrl;
	int ndesc;

	TXQ_LOCK_ASSERT_OWNED(txq);

	ndesc = howmany(txpkts->nflits, 8);

	wr = (void *)&eq->desc[eq->pidx];
	wr->op_immdlen = htobe32(V_FW_WR_OP(FW_ETH_TX_PKTS_WR) |
	    V_FW_WR_IMMDLEN(0)); /* immdlen does not matter in this WR */
	ctrl = V_FW_WR_LEN16(howmany(txpkts->nflits, 2));
	if (eq->avail == ndesc)
		ctrl |= F_FW_WR_EQUEQ | F_FW_WR_EQUIQ;
	wr->equiq_to_len16 = htobe32(ctrl);
	wr->plen = htobe16(txpkts->plen);
	wr->npkt = txpkts->npkt;
	wr->r3 = wr->r4 = 0;

	/* Everything else already written */

	txsd = &eq->sdesc[eq->pidx];
	txsd->desc_used = ndesc;

	KASSERT(eq->avail >= ndesc, ("%s: out ouf descriptors", __func__));

	eq->pending += ndesc;
	eq->avail -= ndesc;
	eq->pidx += ndesc;
	if (eq->pidx >= eq->cap)
		eq->pidx -= eq->cap;

	txq->txpkts_pkts += txpkts->npkt;
	txq->txpkts_wrs++;
	txpkts->npkt = 0;	/* emptied */
}

static inline void
write_ulp_cpl_sgl(struct port_info *pi, struct sge_txq *txq,
    struct txpkts *txpkts, struct mbuf *m, struct sgl *sgl)
{
	struct ulp_txpkt *ulpmc;
	struct ulptx_idata *ulpsc;
	struct cpl_tx_pkt_core *cpl;
	struct sge_eq *eq = &txq->eq;
	uintptr_t flitp, start, end;
	uint64_t ctrl;
	caddr_t dst;

	KASSERT(txpkts->npkt > 0, ("%s: txpkts is empty", __func__));

	start = (uintptr_t)eq->desc;
	end = (uintptr_t)eq->spg;

	/* Checksum offload */
	ctrl = 0;
	if (!(m->m_pkthdr.csum_flags & CSUM_IP))
		ctrl |= F_TXPKT_IPCSUM_DIS;
	if (!(m->m_pkthdr.csum_flags & (CSUM_TCP | CSUM_UDP)))
		ctrl |= F_TXPKT_L4CSUM_DIS;
	if (m->m_pkthdr.csum_flags & (CSUM_IP | CSUM_TCP | CSUM_UDP))
		txq->txcsum++;	/* some hardware assistance provided */

	/* VLAN tag insertion */
	if (m->m_flags & M_VLANTAG) {
		ctrl |= F_TXPKT_VLAN_VLD | V_TXPKT_VLAN(m->m_pkthdr.ether_vtag);
		txq->vlan_insertion++;
	}

	/*
	 * The previous packet's SGL must have ended at a 16 byte boundary (this
	 * is required by the firmware/hardware).  It follows that flitp cannot
	 * wrap around between the ULPTX master command and ULPTX subcommand (8
	 * bytes each), and that it can not wrap around in the middle of the
	 * cpl_tx_pkt_core either.
	 */
	flitp = (uintptr_t)txpkts->flitp;
	KASSERT((flitp & 0xf) == 0,
	    ("%s: last SGL did not end at 16 byte boundary: %p",
	    __func__, txpkts->flitp));

	/* ULP master command */
	ulpmc = (void *)flitp;
	ulpmc->cmd_dest = htonl(V_ULPTX_CMD(ULP_TX_PKT) | V_ULP_TXPKT_DEST(0));
	ulpmc->len = htonl(howmany(sizeof(*ulpmc) + sizeof(*ulpsc) +
	    sizeof(*cpl) + 8 * sgl->nflits, 16));

	/* ULP subcommand */
	ulpsc = (void *)(ulpmc + 1);
	ulpsc->cmd_more = htobe32(V_ULPTX_CMD((u32)ULP_TX_SC_IMM) |
	    F_ULP_TX_SC_MORE);
	ulpsc->len = htobe32(sizeof(struct cpl_tx_pkt_core));

	flitp += sizeof(*ulpmc) + sizeof(*ulpsc);
	if (flitp == end)
		flitp = start;

	/* CPL_TX_PKT */
	cpl = (void *)flitp;
	cpl->ctrl0 = htobe32(V_TXPKT_OPCODE(CPL_TX_PKT) |
	    V_TXPKT_INTF(pi->tx_chan) | V_TXPKT_PF(pi->adapter->pf));
	cpl->pack = 0;
	cpl->len = htobe16(m->m_pkthdr.len);
	cpl->ctrl1 = htobe64(ctrl);

	flitp += sizeof(*cpl);
	if (flitp == end)
		flitp = start;

	/* SGL for this frame */
	dst = (caddr_t)flitp;
	txpkts->nflits += write_sgl_to_txd(eq, sgl, &dst);
	txpkts->flitp = (void *)dst;

	KASSERT(((uintptr_t)dst & 0xf) == 0,
	    ("%s: SGL ends at %p (not a 16 byte boundary)", __func__, dst));
}

/*
 * If the SGL ends on an address that is not 16 byte aligned, this function will
 * add a 0 filled flit at the end.  It returns 1 in that case.
 */
static int
write_sgl_to_txd(struct sge_eq *eq, struct sgl *sgl, caddr_t *to)
{
	__be64 *flitp, *end;
	struct ulptx_sgl *usgl;
	bus_dma_segment_t *seg;
	int i, padded;

	KASSERT(sgl->nsegs > 0 && sgl->nflits > 0,
	    ("%s: bad SGL - nsegs=%d, nflits=%d",
	    __func__, sgl->nsegs, sgl->nflits));

	KASSERT(((uintptr_t)(*to) & 0xf) == 0,
	    ("%s: SGL must start at a 16 byte boundary: %p", __func__, *to));

	flitp = (__be64 *)(*to);
	end = flitp + sgl->nflits;
	seg = &sgl->seg[0];
	usgl = (void *)flitp;

	/*
	 * We start at a 16 byte boundary somewhere inside the tx descriptor
	 * ring, so we're at least 16 bytes away from the status page.  There is
	 * no chance of a wrap around in the middle of usgl (which is 16 bytes).
	 */

	usgl->cmd_nsge = htobe32(V_ULPTX_CMD(ULP_TX_SC_DSGL) |
	    V_ULPTX_NSGE(sgl->nsegs));
	usgl->len0 = htobe32(seg->ds_len);
	usgl->addr0 = htobe64(seg->ds_addr);
	seg++;

	if ((uintptr_t)end <= (uintptr_t)eq->spg) {

		/* Won't wrap around at all */

		for (i = 0; i < sgl->nsegs - 1; i++, seg++) {
			usgl->sge[i / 2].len[i & 1] = htobe32(seg->ds_len);
			usgl->sge[i / 2].addr[i & 1] = htobe64(seg->ds_addr);
		}
		if (i & 1)
			usgl->sge[i / 2].len[1] = htobe32(0);
	} else {

		/* Will wrap somewhere in the rest of the SGL */

		/* 2 flits already written, write the rest flit by flit */
		flitp = (void *)(usgl + 1);
		for (i = 0; i < sgl->nflits - 2; i++) {
			if ((uintptr_t)flitp == (uintptr_t)eq->spg)
				flitp = (void *)eq->desc;
			*flitp++ = get_flit(seg, sgl->nsegs - 1, i);
		}
		end = flitp;
	}

	if ((uintptr_t)end & 0xf) {
		*(uint64_t *)end = 0;
		end++;
		padded = 1;
	} else
		padded = 0;

	if ((uintptr_t)end == (uintptr_t)eq->spg)
		*to = (void *)eq->desc;
	else
		*to = (void *)end;

	return (padded);
}

static inline void
copy_to_txd(struct sge_eq *eq, caddr_t from, caddr_t *to, int len)
{
	if ((uintptr_t)(*to) + len <= (uintptr_t)eq->spg) {
		bcopy(from, *to, len);
		(*to) += len;
	} else {
		int portion = (uintptr_t)eq->spg - (uintptr_t)(*to);

		bcopy(from, *to, portion);
		from += portion;
		portion = len - portion;	/* remaining */
		bcopy(from, (void *)eq->desc, portion);
		(*to) = (caddr_t)eq->desc + portion;
	}
}

static inline void
ring_tx_db(struct adapter *sc, struct sge_eq *eq)
{
	wmb();
	t4_write_reg(sc, MYPF_REG(A_SGE_PF_KDOORBELL),
	    V_QID(eq->cntxt_id) | V_PIDX(eq->pending));
	eq->pending = 0;
}

static int
reclaim_tx_descs(struct sge_eq *eq, int atleast, int howmany)
{
	struct tx_sdesc *txsd;
	struct tx_map *txm, *next_txm;
	unsigned int cidx, can_reclaim, reclaimed, maps, next_map_cidx;

	EQ_LOCK_ASSERT_OWNED(eq);

	cidx = eq->spg->cidx;	/* stable snapshot */
	cidx = be16_to_cpu(cidx);

	if (cidx >= eq->cidx)
		can_reclaim = cidx - eq->cidx;
	else
		can_reclaim = cidx + eq->cap - eq->cidx;

	if (can_reclaim < atleast)
		return (0);

	next_map_cidx = eq->map_cidx;
	next_txm = txm = &eq->maps[next_map_cidx];
	prefetch(txm);

	maps = reclaimed = 0;
	do {
		int ndesc;

		txsd = &eq->sdesc[eq->cidx];
		ndesc = txsd->desc_used;

		/* Firmware doesn't return "partial" credits. */
		KASSERT(can_reclaim >= ndesc,
		    ("%s: unexpected number of credits: %d, %d",
		    __func__, can_reclaim, ndesc));

		maps += txsd->map_used;
		reclaimed += ndesc;

		eq->cidx += ndesc;
		if (eq->cidx >= eq->cap)
			eq->cidx -= eq->cap;

		can_reclaim -= ndesc;

	} while (can_reclaim && reclaimed < howmany);

	eq->avail += reclaimed;
	KASSERT(eq->avail < eq->cap,	/* avail tops out at (cap - 1) */
	    ("%s: too many descriptors available", __func__));

	eq->map_avail += maps;
	KASSERT(eq->map_avail <= eq->map_total,
	    ("%s: too many maps available", __func__));

	prefetch(txm->m);
	while (maps--) {
		next_txm++;
		if (++next_map_cidx == eq->map_total) {
			next_map_cidx = 0;
			next_txm = eq->maps;
		}
		prefetch(next_txm->m);

		bus_dmamap_unload(eq->tx_tag, txm->map);
		m_freem(txm->m);
		txm->m = NULL;

		txm = next_txm;
	}
	eq->map_cidx = next_map_cidx;

	return (reclaimed);
}

static void
write_eqflush_wr(struct sge_eq *eq)
{
	struct fw_eq_flush_wr *wr;
	struct tx_sdesc *txsd;

	EQ_LOCK_ASSERT_OWNED(eq);
	KASSERT(eq->avail > 0, ("%s: no descriptors left.", __func__));

	wr = (void *)&eq->desc[eq->pidx];
	bzero(wr, sizeof(*wr));
	wr->opcode = FW_EQ_FLUSH_WR;
	wr->equiq_to_len16 = htobe32(V_FW_WR_LEN16(sizeof(*wr) / 16) |
	    F_FW_WR_EQUEQ | F_FW_WR_EQUIQ);

	txsd = &eq->sdesc[eq->pidx];
	txsd->desc_used = 1;
	txsd->map_used = 0;

	eq->pending++;
	eq->avail--;
	if (++eq->pidx == eq->cap)
		eq->pidx = 0; 
}

static __be64
get_flit(bus_dma_segment_t *sgl, int nsegs, int idx)
{
	int i = (idx / 3) * 2;

	switch (idx % 3) {
	case 0: {
		__be64 rc;

		rc = htobe32(sgl[i].ds_len);
		if (i + 1 < nsegs)
			rc |= (uint64_t)htobe32(sgl[i + 1].ds_len) << 32;

		return (rc);
	}
	case 1:
		return htobe64(sgl[i].ds_addr);
	case 2:
		return htobe64(sgl[i + 1].ds_addr);
	}

	return (0);
}

static struct mbuf *
get_fl_sdesc_data(struct sge_fl *fl, int len, int flags)
{
	struct fl_sdesc *sd;
	struct mbuf *m;

	sd = &fl->sdesc[fl->cidx];
	FL_LOCK(fl);
	if (++fl->cidx == fl->cap)
		fl->cidx = 0;
	fl->needed++;
	FL_UNLOCK(fl);

	m = sd->m;
	if (m == NULL) {
		m = m_gethdr(M_NOWAIT, MT_NOINIT);
		if (m == NULL)
			return (NULL);
	}
	sd->m = NULL;	/* consumed */

	bus_dmamap_sync(fl->tag[sd->tag_idx], sd->map, BUS_DMASYNC_POSTREAD);
	m_init(m, zone_mbuf, MLEN, M_NOWAIT, MT_DATA, flags);
	if ((flags && len < MINCLSIZE) || (!flags && len <= MLEN))
		bcopy(sd->cl, mtod(m, caddr_t), len);
	else {
		bus_dmamap_unload(fl->tag[sd->tag_idx], sd->map);
		m_cljset(m, sd->cl, FL_BUF_TYPE(sd->tag_idx));
		sd->cl = NULL;	/* consumed */
	}

	m->m_len = min(len, FL_BUF_SIZE(sd->tag_idx));

	return (m);
}

static void
set_fl_tag_idx(struct sge_fl *fl, int mtu)
{
	int i;

	FL_LOCK_ASSERT_OWNED(fl);

	for (i = 0; i < FL_BUF_SIZES - 1; i++) {
		if (FL_BUF_SIZE(i) >= (mtu + FL_PKTSHIFT))
			break;
	}

	fl->tag_idx = i;
}

static int
handle_sge_egr_update(struct adapter *sc, const struct cpl_sge_egr_update *cpl)
{
	unsigned int qid = G_EGR_QID(ntohl(cpl->opcode_qid));
	struct sge *s = &sc->sge;
	struct sge_txq *txq;

	txq = (void *)s->eqmap[qid - s->eq_start];
	taskqueue_enqueue(txq->port->tq, &txq->resume_tx);
	txq->egr_update++;

	return (0);
}
