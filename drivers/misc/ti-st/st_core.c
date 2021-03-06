/*
 *  Shared Transport Line discipline driver Core
 *	This hooks up ST KIM driver and ST LL driver
 *  Copyright (C) 2009 Texas Instruments
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/tty.h>

/* understand BT, FM and GPS for now */
#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>
#include <net/bluetooth/hci.h>
#include "fm.h"
/*
 * packet formats for fm and gps
 * #include "gps.h"
 */
#include "st_core.h"
#include "st_kim.h"
#include "st_ll.h"
#include "st.h"

/* all debug macros go in here */
#define ST_DRV_ERR(fmt, arg...)  printk(KERN_ERR "(stc):"fmt"\n" , ## arg)
#if defined(DEBUG)		/* limited debug messages */
#define ST_DRV_DBG(fmt, arg...)  printk(KERN_INFO "(stc):"fmt"\n" , ## arg)
#define ST_DRV_VER(fmt, arg...)
#elif defined(VERBOSE)		/* very verbose */
#define ST_DRV_DBG(fmt, arg...)  printk(KERN_INFO "(stc):"fmt"\n" , ## arg)
#define ST_DRV_VER(fmt, arg...)  printk(KERN_INFO "(stc):"fmt"\n" , ## arg)
#else /* error msgs only */
#define ST_DRV_DBG(fmt, arg...)
#define ST_DRV_VER(fmt, arg...)
#endif

#ifdef DEBUG
/* strings to be used for rfkill entries and by
 * ST Core to be used for sysfs debug entry
 */
#define PROTO_ENTRY(type, name)	name
const unsigned char *protocol_strngs[] = {
	PROTO_ENTRY(ST_BT, "Bluetooth"),
	PROTO_ENTRY(ST_FM, "FM"),
	PROTO_ENTRY(ST_GPS, "GPS"),
};
#endif
/*
 * local data instances
 */
static struct st_data_s *st_gdata;
/* function pointer pointing to either,
 * st_kim_recv during registration to receive fw download responses
 * st_int_recv after registration to receive proto stack responses
 */
void (*st_recv) (const unsigned char *data, long count);

/********************************************************************/
/* internal misc functions */
bool is_protocol_list_empty(void)
{
	unsigned char i = 0;
	ST_DRV_DBG(" %s ", __func__);
	for (i = 0; i < ST_MAX; i++) {
		if (st_gdata->list[i] != NULL) {
			return ST_NOTEMPTY;
		}
		/* not empty */
	}
	/* list empty */
	return ST_EMPTY;
}

/* called from KIM during firmware download.
 *
 * This is a wrapper function to tty->ops->write_room.
 * It returns number of free space available in
 * uart tx buffer.
 */
int st_get_uart_wr_room(void)
{
	struct tty_struct *tty;
	if (unlikely(st_gdata == NULL || st_gdata->tty == NULL)) {
		ST_DRV_ERR("tty unavailable to perform write");
		return ST_ERR_FAILURE;
	}
	tty = st_gdata->tty;
	return tty->ops->write_room(tty);
}

/* can be called in from
 * -- KIM (during fw download)
 * -- ST Core (during st_write)
 *
 *  This is the internal write function - a wrapper
 *  to tty->ops->write
 */
int st_int_write(const unsigned char *data, int count)
{
#ifdef VERBOSE			/* for debug */
	int i;
#endif
	struct tty_struct *tty;
	if (unlikely(st_gdata == NULL || st_gdata->tty == NULL)) {
		ST_DRV_ERR("tty unavailable to perform write");
		return ST_ERR_FAILURE;
	}
	tty = st_gdata->tty;
#ifdef VERBOSE
	printk(KERN_ERR "start data.. \n");
	for (i = 0; i < count; i++)	/* no newlines for each datum */
		printk(" %x", data[i]);
	printk(KERN_ERR "\n ..end data\n");
#endif

	return tty->ops->write(tty, data, count);

}

/*
 * push the skb received to relevant
 * protocol stacks
 */
void st_send_frame(enum proto_type protoid, struct sk_buff *skb)
{
	ST_DRV_DBG(" %s(prot:%d) ", __func__, protoid);

	if (unlikely
	    (st_gdata == NULL || skb == NULL
	     || st_gdata->list[protoid] == NULL)) {
		ST_DRV_ERR("protocol %d not registered, no data to send?",
			   protoid);
		kfree_skb(skb);
		return;
	}
	/* this cannot fail
	 * this shouldn't take long
	 * - should be just skb_queue_tail for the
	 *   protocol stack driver
	 */
	if (likely(st_gdata->list[protoid]->recv != NULL)) {
		if (unlikely(st_gdata->list[protoid]->recv(skb)
			     != ST_SUCCESS)) {
			ST_DRV_ERR(" proto stack %d's ->recv failed", protoid);
			kfree_skb(skb);
			return;
		}
	} else {
		ST_DRV_ERR(" proto stack %d's ->recv null", protoid);
		kfree_skb(skb);
	}
	ST_DRV_DBG(" done %s", __func__);
	return;
}

/*
 * to call registration complete callbacks
 * of all protocol stack drivers
 */
void st_reg_complete(char err)
{
	unsigned char i = 0;
	ST_DRV_DBG(" %s ", __func__);
	for (i = 0; i < ST_MAX; i++) {
		if (likely(st_gdata != NULL && st_gdata->list[i] != NULL &&
			   st_gdata->list[i]->reg_complete_cb != NULL))
			st_gdata->list[i]->reg_complete_cb(err);
	}
}

static inline int st_check_data_len(int protoid, int len)
{
	register int room = skb_tailroom(st_gdata->rx_skb);

	ST_DRV_DBG("len %d room %d", len, room);

	if (!len) {
		/* Received packet has only packet header and
		 * has zero length payload. So, ask ST CORE to
		 * forward the packet to protocol driver (BT/FM/GPS)
		 */
		st_send_frame(protoid, st_gdata->rx_skb);

	} else if (len > room) {
		/* Received packet's payload length is larger.
		 * We can't accommodate it in created skb.
		 */
		ST_DRV_ERR("Data length is too large len %d room %d", len,
			   room);
		kfree_skb(st_gdata->rx_skb);
	} else {
		/* Packet header has non-zero payload length and
		 * we have enough space in created skb. Lets read
		 * payload data */
		st_gdata->rx_state = ST_BT_W4_DATA;
		st_gdata->rx_count = len;
		return len;
	}

	/* Change ST state to continue to process next
	 * packet */
	st_gdata->rx_state = ST_W4_PACKET_TYPE;
	st_gdata->rx_skb = NULL;
	st_gdata->rx_count = 0;

	return 0;
}

/* internal function for action when wake-up ack
 * received
 */
static inline void st_wakeup_ack(unsigned char cmd)
{
	register struct sk_buff *waiting_skb;
	unsigned long flags = 0;

	spin_lock_irqsave(&st_gdata->lock, flags);
	/* de-Q from waitQ and Q in txQ now that the
	 * chip is awake
	 */
	while ((waiting_skb = skb_dequeue(&st_gdata->tx_waitq)))
		skb_queue_tail(&st_gdata->txq, waiting_skb);

	/* state forwarded to ST LL */
	st_ll_sleep_state((unsigned long)cmd);
	spin_unlock_irqrestore(&st_gdata->lock, flags);

	/* wake up to send the recently copied skbs from waitQ */
	st_tx_wakeup(st_gdata);
}

/* Decodes received RAW data and forwards to corresponding
 * client drivers (Bluetooth,FM,GPS..etc).
 *
 */
void st_int_recv(const unsigned char *data, long count)
{
	register char *ptr;
	struct hci_event_hdr *eh;
	struct hci_acl_hdr *ah;
	struct hci_sco_hdr *sh;
	struct fm_event_hdr *fm;
	struct gps_event_hdr *gps;
	register int len = 0, type = 0, dlen = 0;
	static enum proto_type protoid = ST_MAX;

	ST_DRV_DBG("count %ld rx_state %ld"
		   "rx_count %ld", count, st_gdata->rx_state,
		   st_gdata->rx_count);

	ptr = (char *)data;
	/* tty_receive sent null ? */
	if (unlikely(ptr == NULL)) {
		ST_DRV_ERR(" received null from TTY ");
		return;
	}

	/* Decode received bytes here */
	while (count) {
		if (st_gdata->rx_count) {
			len = min_t(unsigned int, st_gdata->rx_count, count);
			memcpy(skb_put(st_gdata->rx_skb, len), ptr, len);
			st_gdata->rx_count -= len;
			count -= len;
			ptr += len;

			if (st_gdata->rx_count)
				continue;

			/* Check ST RX state machine , where are we? */
			switch (st_gdata->rx_state) {

				/* Waiting for complete packet ? */
			case ST_BT_W4_DATA:
				ST_DRV_DBG("Complete pkt received");

				/* Ask ST CORE to forward
				 * the packet to protocol driver */
				st_send_frame(protoid, st_gdata->rx_skb);

				st_gdata->rx_state = ST_W4_PACKET_TYPE;
				st_gdata->rx_skb = NULL;
				protoid = ST_MAX;	/* is this required ? */
				continue;

				/* Waiting for Bluetooth event header ? */
			case ST_BT_W4_EVENT_HDR:
				eh = (struct hci_event_hdr *)st_gdata->rx_skb->
				    data;

				ST_DRV_DBG("Event header: evt 0x%2.2x"
					   "plen %d", eh->evt, eh->plen);

				st_check_data_len(protoid, eh->plen);
				continue;

				/* Waiting for Bluetooth acl header ? */
			case ST_BT_W4_ACL_HDR:
				ah = (struct hci_acl_hdr *)st_gdata->rx_skb->
				    data;
				dlen = __le16_to_cpu(ah->dlen);

				ST_DRV_DBG("ACL header: dlen %d", dlen);

				st_check_data_len(protoid, dlen);
				continue;

				/* Waiting for Bluetooth sco header ? */
			case ST_BT_W4_SCO_HDR:
				sh = (struct hci_sco_hdr *)st_gdata->rx_skb->
				    data;

				ST_DRV_DBG("SCO header: dlen %d", sh->dlen);

				st_check_data_len(protoid, sh->dlen);
				continue;
			case ST_FM_W4_EVENT_HDR:
				fm = (struct fm_event_hdr *)st_gdata->rx_skb->
				    data;
				ST_DRV_DBG("FM Header: ");
				st_check_data_len(ST_FM, fm->plen);
				continue;
				/* TODO : Add GPS packet machine logic here */
			case ST_GPS_W4_EVENT_HDR:
				/* [0x09 pkt hdr][R/W byte][2 byte len] */
				gps = (struct gps_event_hdr *)st_gdata->rx_skb->
				     data;
				ST_DRV_DBG("GPS Header: ");
				st_check_data_len(ST_GPS, gps->plen);
				continue;
			}	/* end of switch rx_state */
		}

		/* end of if rx_count */
		/* Check first byte of packet and identify module
		 * owner (BT/FM/GPS) */
		switch (*ptr) {

			/* Bluetooth event packet? */
		case HCI_EVENT_PKT:
			ST_DRV_DBG("Event packet");
			st_gdata->rx_state = ST_BT_W4_EVENT_HDR;
			st_gdata->rx_count = HCI_EVENT_HDR_SIZE;
			type = HCI_EVENT_PKT;
			protoid = ST_BT;
			break;

			/* Bluetooth acl packet? */
		case HCI_ACLDATA_PKT:
			ST_DRV_DBG("ACL packet");
			st_gdata->rx_state = ST_BT_W4_ACL_HDR;
			st_gdata->rx_count = HCI_ACL_HDR_SIZE;
			type = HCI_ACLDATA_PKT;
			protoid = ST_BT;
			break;

			/* Bluetooth sco packet? */
		case HCI_SCODATA_PKT:
			ST_DRV_DBG("SCO packet");
			st_gdata->rx_state = ST_BT_W4_SCO_HDR;
			st_gdata->rx_count = HCI_SCO_HDR_SIZE;
			type = HCI_SCODATA_PKT;
			protoid = ST_BT;
			break;

			/* Channel 8(FM) packet? */
		case ST_FM_CH8_PKT:
			ST_DRV_DBG("FM CH8 packet");
			type = ST_FM_CH8_PKT;
			st_gdata->rx_state = ST_FM_W4_EVENT_HDR;
			st_gdata->rx_count = FM_EVENT_HDR_SIZE;
			protoid = ST_FM;
			break;

			/* Channel 9(GPS) packet? */
		case 0x9:	/*ST_LL_GPS_CH9_PKT */
			ST_DRV_DBG("GPS CH9 packet");
			type = 0x9;	/* ST_LL_GPS_CH9_PKT; */
			protoid = ST_GPS;
			st_gdata->rx_state = ST_GPS_W4_EVENT_HDR;
			st_gdata->rx_count = 3;	/* GPS_EVENT_HDR_SIZE -1*/
			break;
		case LL_SLEEP_IND:
		case LL_SLEEP_ACK:
		case LL_WAKE_UP_IND:
			/* this takes appropriate action based on
			 * sleep state received --
			 */
			st_ll_sleep_state(*ptr);
			ptr++;
			count--;
			continue;
		case LL_WAKE_UP_ACK:
			/* wake up ack received */
			st_wakeup_ack(*ptr);
			ptr++;
			count--;
			continue;
			/* Unknow packet? */
		default:
			ST_DRV_ERR("Unknown packet type %2.2x", (__u8) *ptr);
			ptr++;
			count--;
			continue;
		};
		ptr++;
		count--;

		switch (protoid) {
		case ST_BT:
			/* Allocate new packet to hold received data */
			st_gdata->rx_skb =
			    bt_skb_alloc(HCI_MAX_FRAME_SIZE, GFP_ATOMIC);
			if (!st_gdata->rx_skb) {
				ST_DRV_ERR("Can't allocate mem for new packet");
				st_gdata->rx_state = ST_W4_PACKET_TYPE;
				st_gdata->rx_count = 0;
				return;
			}
			bt_cb(st_gdata->rx_skb)->pkt_type = type;
			break;
		case ST_FM:	/* for FM */
			st_gdata->rx_skb =
			    alloc_skb(FM_MAX_FRAME_SIZE, GFP_ATOMIC);
			if (!st_gdata->rx_skb) {
				ST_DRV_ERR("Can't allocate mem for new packet");
				st_gdata->rx_state = ST_W4_PACKET_TYPE;
				st_gdata->rx_count = 0;
				return;
			}
			/* place holder 0x08 */
			skb_reserve(st_gdata->rx_skb, 1);
			st_gdata->rx_skb->cb[0] = ST_FM_CH8_PKT;
			break;
		case ST_GPS:
			/* for GPS */
			st_gdata->rx_skb =
			    alloc_skb(100 /*GPS_MAX_FRAME_SIZE */ , GFP_ATOMIC);
			if (!st_gdata->rx_skb) {
				ST_DRV_ERR("Can't allocate mem for new packet");
				st_gdata->rx_state = ST_W4_PACKET_TYPE;
				st_gdata->rx_count = 0;
				return;
			}
			/* place holder 0x09 */
			skb_reserve(st_gdata->rx_skb, 1);
			st_gdata->rx_skb->cb[0] = 0x09;	/*ST_GPS_CH9_PKT; */
			break;
		case ST_MAX:
			break;
		}
	}
	ST_DRV_DBG("done %s", __func__);
	return;
}

/* internal de-Q function
 * -- return previous in-completely written skb
 *  or return the skb in the txQ
 */
struct sk_buff *st_int_dequeue(struct st_data_s *st_data)
{
	struct sk_buff *returning_skb;

	ST_DRV_VER("%s", __func__);
	/* if the previous skb wasn't written completely
	 */
	if (st_gdata->tx_skb != NULL) {
		returning_skb = st_gdata->tx_skb;
		st_gdata->tx_skb = NULL;
		return returning_skb;
	}

	/* de-Q from the txQ always if previous write is complete */
	return skb_dequeue(&st_gdata->txq);
}

/* internal Q-ing function
 * will either Q the skb to txq or the tx_waitq
 * depending on the ST LL state
 *
 * lock the whole func - since ll_getstate and Q-ing should happen
 * in one-shot
 */
void st_int_enqueue(struct sk_buff *skb)
{
	unsigned long flags = 0;

	ST_DRV_VER("%s", __func__);
	/* this function can be invoked in more then one context.
	 * so have a lock */
	spin_lock_irqsave(&st_gdata->lock, flags);

	switch (st_ll_getstate()) {
	case ST_LL_AWAKE:
		ST_DRV_DBG("ST LL is AWAKE, sending normally");
		skb_queue_tail(&st_gdata->txq, skb);
		break;
	case ST_LL_ASLEEP_TO_AWAKE:
		skb_queue_tail(&st_gdata->tx_waitq, skb);
		break;
	case ST_LL_AWAKE_TO_ASLEEP:	/* host cannot be in this state */
		ST_DRV_ERR("ST LL is illegal state(%ld),"
			   "purging received skb.", st_ll_getstate());
		kfree_skb(skb);
		break;

	case ST_LL_ASLEEP:
		/* call a function of ST LL to put data
		 * in tx_waitQ and wake_ind in txQ
		 */
		skb_queue_tail(&st_gdata->tx_waitq, skb);
		st_ll_wakeup();
		break;
	default:
		ST_DRV_ERR("ST LL is illegal state(%ld),"
			   "purging received skb.", st_ll_getstate());
		kfree_skb(skb);
		break;
	}
	spin_unlock_irqrestore(&st_gdata->lock, flags);
	ST_DRV_VER("done %s", __func__);
	return;
}

/*
 * internal wakeup function
 * called from either
 * - TTY layer when write's finished
 * - st_write (in context of the protocol stack)
 */
void st_tx_wakeup(struct st_data_s *st_data)
{
	struct sk_buff *skb;
	unsigned long flags;	/* for irq save flags */
	ST_DRV_VER("%s", __func__);
	/* check for sending & set flag sending here */
	if (test_and_set_bit(ST_TX_SENDING, &st_data->tx_state)) {
		ST_DRV_DBG("ST already sending");
		/* keep sending */
		set_bit(ST_TX_WAKEUP, &st_data->tx_state);
		return;
		/* TX_WAKEUP will be checked in another
		 * context
		 */
	}
	do {			/* come back if st_tx_wakeup is set */
		/* woke-up to write */
		clear_bit(ST_TX_WAKEUP, &st_data->tx_state);
		while ((skb = st_int_dequeue(st_data))) {
			int len;
			spin_lock_irqsave(&st_data->lock, flags);
			/* enable wake-up from TTY */
			set_bit(TTY_DO_WRITE_WAKEUP, &st_data->tty->flags);
			len = st_int_write(skb->data, skb->len);
			skb_pull(skb, len);
			/* if skb->len = len as expected, skb->len=0 */
			if (skb->len) {
				/* would be the next skb to be sent */
				st_data->tx_skb = skb;
				spin_unlock_irqrestore(&st_gdata->lock, flags);
				break;
			}
			kfree_skb(skb);
			spin_unlock_irqrestore(&st_gdata->lock, flags);
		}
		/* if wake-up is set in another context- restart sending */
	} while (test_bit(ST_TX_WAKEUP, &st_data->tx_state));

	/* clear flag sending */
	clear_bit(ST_TX_SENDING, &st_data->tx_state);
}

/********************************************************************/
/* functions called from ST KIM
*/
void kim_st_list_protocols(char *buf)
{
	unsigned long flags = 0;
#ifdef DEBUG
	unsigned char i = ST_MAX;
#endif
	spin_lock_irqsave(&st_gdata->lock, flags);
#ifdef DEBUG			/* more detailed log */
	for (i = 0; i < ST_MAX; i++) {
		if (i == 0) {
			sprintf(buf, "%s is %s", protocol_strngs[i],
				st_gdata->list[i] !=
				NULL ? "Registered" : "Unregistered");
		} else {
			sprintf(buf, "%s\n%s is %s", buf, protocol_strngs[i],
				st_gdata->list[i] !=
				NULL ? "Registered" : "Unregistered");
		}
	}
	sprintf(buf, "%s\n", buf);
#else /* limited info */
	sprintf(buf, "BT=%c\nFM=%c\nGPS=%c\n",
		st_gdata->list[ST_BT] != NULL ? 'R' : 'U',
		st_gdata->list[ST_FM] != NULL ? 'R' : 'U',
		st_gdata->list[ST_GPS] != NULL ? 'R' : 'U');
#endif
	spin_unlock_irqrestore(&st_gdata->lock, flags);
}

/********************************************************************/
/*
 * functions called from protocol stack drivers
 * to be EXPORT-ed
 */
long st_register(struct st_proto_s *new_proto)
{
	long err = ST_SUCCESS;
	unsigned long flags = 0;

	ST_DRV_DBG("%s(%d) ", __func__, new_proto->type);
	if (st_gdata == NULL || new_proto == NULL || new_proto->recv == NULL
	    || new_proto->reg_complete_cb == NULL) {
		ST_DRV_ERR("gdata/new_proto/recv or reg_complete_cb not ready");
		return ST_ERR_FAILURE;
	}

	if (new_proto->type < ST_BT || new_proto->type >= ST_MAX) {
		ST_DRV_ERR("protocol %d not supported", new_proto->type);
		return ST_ERR_NOPROTO;
	}

	if (st_gdata->list[new_proto->type] != NULL) {
		ST_DRV_ERR("protocol %d already registered", new_proto->type);
		return ST_ERR_ALREADY;
	}

	/* can be from process context only */
	spin_lock_irqsave(&st_gdata->lock, flags);

	if (test_bit(ST_REG_IN_PROGRESS, &st_gdata->st_state)) {
		ST_DRV_DBG(" ST_REG_IN_PROGRESS:%d ", new_proto->type);
		/* fw download in progress */
		st_kim_chip_toggle(new_proto->type, KIM_GPIO_ACTIVE);

		st_gdata->list[new_proto->type] = new_proto;
		new_proto->write = st_write;

		set_bit(ST_REG_PENDING, &st_gdata->st_state);
		spin_unlock_irqrestore(&st_gdata->lock, flags);
		return ST_ERR_PENDING;
	} else if (is_protocol_list_empty() == ST_EMPTY) {
		ST_DRV_DBG(" protocol list empty :%d ", new_proto->type);
		set_bit(ST_REG_IN_PROGRESS, &st_gdata->st_state);
		st_recv = st_kim_recv;

		/* release lock previously held - re-locked below */
		spin_unlock_irqrestore(&st_gdata->lock, flags);

		/* enable the ST LL - to set default chip state */
		st_ll_enable();
		/* this may take a while to complete
		 * since it involves BT fw download
		 */
		err = st_kim_start();
		if (err != ST_SUCCESS) {
			clear_bit(ST_REG_IN_PROGRESS, &st_gdata->st_state);
			if ((is_protocol_list_empty() != ST_EMPTY) &&
			    (test_bit(ST_REG_PENDING, &st_gdata->st_state))) {
				ST_DRV_ERR(" KIM failure complete callback ");
				st_reg_complete(ST_ERR_FAILURE);
			}

			return ST_ERR_FAILURE;
		}

		/* the protocol might require other gpios to be toggled
		 */
		st_kim_chip_toggle(new_proto->type, KIM_GPIO_ACTIVE);

		clear_bit(ST_REG_IN_PROGRESS, &st_gdata->st_state);
		st_recv = st_int_recv;

		/* this is where all pending registration
		 * are signalled to be complete by calling callback functions
		 */
		if ((is_protocol_list_empty() != ST_EMPTY) &&
		    (test_bit(ST_REG_PENDING, &st_gdata->st_state))) {
			ST_DRV_VER(" call reg complete callback ");
			st_reg_complete(ST_SUCCESS);
		}
		clear_bit(ST_REG_PENDING, &st_gdata->st_state);

		/* check for already registered once more,
		 * since the above check is old
		 */
		if (st_gdata->list[new_proto->type] != NULL) {
			ST_DRV_ERR(" proto %d already registered ",
				   new_proto->type);
			return ST_ERR_ALREADY;
		}

		spin_lock_irqsave(&st_gdata->lock, flags);
		st_gdata->list[new_proto->type] = new_proto;
		new_proto->write = st_write;
		spin_unlock_irqrestore(&st_gdata->lock, flags);
		return err;
	}
	/* if fw is already downloaded & new stack registers protocol */
	else {
		switch (new_proto->type) {
		case ST_BT:
			/* do nothing */
			break;
		case ST_FM:
		case ST_GPS:
			st_kim_chip_toggle(new_proto->type, KIM_GPIO_ACTIVE);
			break;
		case ST_MAX:
		default:
			ST_DRV_ERR("%d protocol not supported",
				   new_proto->type);
			err = ST_ERR_NOPROTO;
			/* something wrong */
			break;
		}
		st_gdata->list[new_proto->type] = new_proto;
		new_proto->write = st_write;

		/* lock already held before entering else */
		spin_unlock_irqrestore(&st_gdata->lock, flags);
		return err;
	}
	ST_DRV_DBG("done %s(%d) ", __func__, new_proto->type);
}
EXPORT_SYMBOL_GPL(st_register);

/* to unregister a protocol -
 * to be called from protocol stack driver
 */
long st_unregister(enum proto_type type)
{
	long err = ST_SUCCESS;
	unsigned long flags = 0;

	ST_DRV_DBG("%s: %d ", __func__, type);

	if (type < ST_BT || type >= ST_MAX) {
		ST_DRV_ERR(" protocol %d not supported", type);
		return ST_ERR_NOPROTO;
	}

	spin_lock_irqsave(&st_gdata->lock, flags);

	if (st_gdata->list[type] == NULL) {
		ST_DRV_ERR(" protocol %d not registered", type);
		spin_unlock_irqrestore(&st_gdata->lock, flags);
		return ST_ERR_NOPROTO;
	}

	st_gdata->list[type] = NULL;

	/* kim ignores BT in the below function
	 * and handles the rest, BT is toggled
	 * only in kim_start and kim_stop
	 */
	st_kim_chip_toggle(type, KIM_GPIO_INACTIVE);
	spin_unlock_irqrestore(&st_gdata->lock, flags);

	if ((is_protocol_list_empty() == ST_EMPTY) &&
	    (!test_bit(ST_REG_PENDING, &st_gdata->st_state))) {
		ST_DRV_DBG(" all protocols unregistered ");

		/* stop traffic on tty */
		if (st_gdata->tty) {
			tty_ldisc_flush(st_gdata->tty);
			stop_tty(st_gdata->tty);
		}

		/* all protocols now unregistered */
		st_kim_stop();
		/* disable ST LL */
		st_ll_disable();
	}
	return err;
}

/*
 * called in protocol stack drivers
 * via the write function pointer
 */
long st_write(struct sk_buff *skb)
{
#ifdef DEBUG
	enum proto_type protoid = ST_MAX;
#endif
	long len;
	struct st_data_s *st_data = st_gdata;

	if (unlikely(skb == NULL || st_gdata == NULL
		|| st_gdata->tty == NULL)) {
		ST_DRV_ERR("data/tty unavailable to perform write");
		return ST_ERR_FAILURE;
	}
#ifdef DEBUG			/* open-up skb to read the 1st byte */
	switch (skb->data[0]) {
	case HCI_COMMAND_PKT:
	case HCI_ACLDATA_PKT:
	case HCI_SCODATA_PKT:
		protoid = ST_BT;
		break;
	case ST_FM_CH8_PKT:
		protoid = ST_FM;
		break;
	case 0x09:
		protoid = ST_GPS;
		break;
	}
	if (unlikely(st_gdata->list[protoid] == NULL)) {
		ST_DRV_ERR(" protocol %d not registered, and writing? ",
			   protoid);
		return ST_ERR_FAILURE;
	}
#endif
	ST_DRV_DBG("%d to be written", skb->len);
	len = skb->len;

	/* st_ll to decide where to enqueue the skb */
	st_int_enqueue(skb);
	/* wake up */
	st_tx_wakeup(st_data);

	/* return number of bytes written */
	return len;
}

/* for protocols making use of shared transport */
EXPORT_SYMBOL_GPL(st_unregister);

/********************************************************************/
/*
 * functions called from TTY layer
 */
static int st_tty_open(struct tty_struct *tty)
{
	int err = ST_SUCCESS;
	ST_DRV_DBG("%s ", __func__);

	st_gdata->tty = tty;

	/* don't do an wakeup for now */
	clear_bit(TTY_DO_WRITE_WAKEUP, &tty->flags);

	/* mem already allocated
	 */
	tty->receive_room = 65536;
	/* Flush any pending characters in the driver and discipline. */
	tty_ldisc_flush(tty);
	tty_driver_flush_buffer(tty);
	/*
	 * signal to UIM via KIM that -
	 * installation of N_SHARED ldisc is complete
	 */
	st_kim_complete();
	ST_DRV_DBG("done %s", __func__);
	return err;
}

static void st_tty_close(struct tty_struct *tty)
{
	unsigned char i = ST_MAX;
	unsigned long flags = 0;

	ST_DRV_DBG("%s ", __func__);

	/* TODO:
	 * if a protocol has been registered & line discipline
	 * un-installed for some reason - what should be done ?
	 */
	spin_lock_irqsave(&st_gdata->lock, flags);
	for (i = ST_BT; i < ST_MAX; i++) {
		if (st_gdata->list[i] != NULL)
			ST_DRV_ERR("%d not un-registered", i);
		st_gdata->list[i] = NULL;
	}
	spin_unlock_irqrestore(&st_gdata->lock, flags);
	/*
	 * signal to UIM via KIM that -
	 * N_SHARED ldisc is un-installed
	 */
	st_kim_complete();
	st_gdata->tty = NULL;
	/* Flush any pending characters in the driver and discipline. */
	tty_ldisc_flush(tty);
	tty_driver_flush_buffer(tty);

	spin_lock_irqsave(&st_gdata->lock, flags);
	/* empty out txq and tx_waitq */
	skb_queue_purge(&st_gdata->txq);
	skb_queue_purge(&st_gdata->tx_waitq);
	/* reset the TTY Rx states of ST */
	st_gdata->rx_count = 0;
	st_gdata->rx_state = ST_W4_PACKET_TYPE;
	kfree_skb(st_gdata->rx_skb);
	st_gdata->rx_skb = NULL;
	spin_unlock_irqrestore(&st_gdata->lock, flags);

	ST_DRV_DBG("%s: done ", __func__);
}

static void st_tty_receive(struct tty_struct *tty, const unsigned char *data,
			   char *tty_flags, int count)
{
	unsigned long flags = 0;

#ifdef VERBOSE
	long i;
	printk(KERN_ERR "incoming data...\n");
	for (i = 0; i < count; i++)
		printk(" %x", data[i]);
	printk(KERN_ERR "\n.. data end\n");
#endif

	/*
	 * if fw download is in progress then route incoming data
	 * to KIM for validation
	 */
	spin_lock_irqsave(&st_gdata->lock, flags);
	st_recv(data, count);
	spin_unlock_irqrestore(&st_gdata->lock, flags);

	ST_DRV_VER("done %s", __func__);
}

/* wake-up function called in from the TTY layer
 * inside the internal wakeup function will be called
 */
static void st_tty_wakeup(struct tty_struct *tty)
{
	ST_DRV_DBG("%s ", __func__);
	/* don't do an wakeup for now */
	clear_bit(TTY_DO_WRITE_WAKEUP, &tty->flags);

	/* call our internal wakeup */
	st_tx_wakeup((void *)st_gdata);
}

static void st_tty_flush_buffer(struct tty_struct *tty)
{
	ST_DRV_DBG("%s ", __func__);

	kfree_skb(st_gdata->tx_skb);
	st_gdata->tx_skb = NULL;

	tty->ops->flush_buffer(tty);
	return;
}

/********************************************************************/
static int __init st_core_init(void)
{
	long err;
	static struct tty_ldisc_ops *st_ldisc_ops;

	/* populate and register to TTY line discipline */
	st_ldisc_ops = kzalloc(sizeof(*st_ldisc_ops), GFP_KERNEL);
	if (!st_ldisc_ops) {
		ST_DRV_ERR("no mem to allocate");
		return -ENOMEM;
	}

	st_ldisc_ops->magic = TTY_LDISC_MAGIC;
	st_ldisc_ops->name = "n_st";	/*"n_hci"; */
	st_ldisc_ops->open = st_tty_open;
	st_ldisc_ops->close = st_tty_close;
	st_ldisc_ops->receive_buf = st_tty_receive;
	st_ldisc_ops->write_wakeup = st_tty_wakeup;
	st_ldisc_ops->flush_buffer = st_tty_flush_buffer;
	st_ldisc_ops->owner = THIS_MODULE;

	err = tty_register_ldisc(N_SHARED, st_ldisc_ops);
	if (err) {
		ST_DRV_ERR("error registering %d line discipline %ld",
			   N_SHARED, err);
		kfree(st_ldisc_ops);
		return err;
	}
	ST_DRV_DBG("registered n_shared line discipline");

	st_gdata = kzalloc(sizeof(struct st_data_s), GFP_KERNEL);
	if (!st_gdata) {
		ST_DRV_ERR("memory allocation failed");
		err = tty_unregister_ldisc(N_SHARED);
		if (err)
			ST_DRV_ERR("unable to un-register ldisc %ld", err);
		kfree(st_ldisc_ops);
		err = -ENOMEM;
		return err;
	}

	/* Initialize ST TxQ and Tx waitQ queue head. All BT/FM/GPS module skb's
	 * will be pushed in this queue for actual transmission.
	 */
	skb_queue_head_init(&st_gdata->txq);
	skb_queue_head_init(&st_gdata->tx_waitq);

	/* Locking used in st_int_enqueue() to avoid multiple execution */
	spin_lock_init(&st_gdata->lock);

	/* ldisc_ops ref to be only used in __exit of module */
	st_gdata->ldisc_ops = st_ldisc_ops;

	err = st_kim_init();
	if (err) {
		ST_DRV_ERR("error during kim initialization(%ld)", err);
		kfree(st_gdata);
		err = tty_unregister_ldisc(N_SHARED);
		if (err)
			ST_DRV_ERR("unable to un-register ldisc");
		kfree(st_ldisc_ops);
		return -1;
	}

	err = st_ll_init();
	if (err) {
		ST_DRV_ERR("error during st_ll initialization(%ld)", err);
		err = st_kim_deinit();
		kfree(st_gdata);
		err = tty_unregister_ldisc(N_SHARED);
		if (err)
			ST_DRV_ERR("unable to un-register ldisc");
		kfree(st_ldisc_ops);
		return -1;
	}
	return 0;
}

static void __exit st_core_exit(void)
{
	long err;
	/* internal module cleanup */
	err = st_ll_deinit();
	if (err)
		ST_DRV_ERR("error during deinit of ST LL %ld", err);
	err = st_kim_deinit();
	if (err)
		ST_DRV_ERR("error during deinit of ST KIM %ld", err);

	if (st_gdata != NULL) {
		/* Free ST Tx Qs and skbs */
		skb_queue_purge(&st_gdata->txq);
		skb_queue_purge(&st_gdata->tx_waitq);
		kfree_skb(st_gdata->rx_skb);
		kfree_skb(st_gdata->tx_skb);
		/* TTY ldisc cleanup */
		err = tty_unregister_ldisc(N_SHARED);
		if (err)
			ST_DRV_ERR("unable to un-register ldisc %ld", err);
		kfree(st_gdata->ldisc_ops);
		/* free the global data pointer */
		kfree(st_gdata);
	}
}

module_init(st_core_init);
module_exit(st_core_exit);
MODULE_AUTHOR("Pavan Savoy <pavan_savoy@ti.com>");
MODULE_DESCRIPTION("Shared Transport Driver for TI BT/FM/GPS combo chips ");
MODULE_LICENSE("GPL");
