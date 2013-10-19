/*
 * \brief  Nic session to DDE-Kit wifi adaptor
 * \author Stefan Kalkowski
 * \date   2009-11-17
 */

/* Genode base framework */
#include <base/env.h>
#include <base/sleep.h>
#include <util/arg_string.h>
#include <util/misc_math.h>
#include <root/component.h>
#include <cap_session/connection.h>
#include <nic_session/rpc_object.h>
#include <nic_session/client.h>
#include <timer_session/connection.h>
#include <os/config.h>

/* DDE kit */
extern "C" {
#include <dde_kit/lock.h>
#include <dde_kit/printf.h>
#include <dde_kit/timer.h>
#include <dde_linux26/general.h>
#include <dde_linux26/net.h>
#include <dde_linux26/sleep.h>
#include <dde_linux26/wifi.h>
}

extern int (*dde_kit_initcall_1_dde_linux26_page_cache_init)(void);
extern int (*dde_kit_initcall_1_helper_init)(void);
extern int (*dde_kit_initcall_2_pci_driver_init)(void);
extern int (*dde_kit_initcall_2_pcibus_class_init)(void);
extern int (*dde_kit_initcall_4__call_init_workqueues)(void);
extern int (*dde_kit_initcall_4_dde_linux26_init_pci)(void);
extern int (*dde_kit_initcall_4_net_dev_init)(void);
extern int (*dde_kit_initcall_4_wireless_nlevent_init)(void);
extern int (*dde_kit_initcall_6_pci_init)(void);
extern int (*dde_kit_initcall_6_init_ath_hal)(void);
extern int (*dde_kit_initcall_6_init_ath_pci)(void);
extern int (*dde_kit_initcall_6_init_ath_rate_sample)(void);
extern int (*dde_kit_initcall_6_init_crypto_ccmp)(void);
extern int (*dde_kit_initcall_6_init_crypto_tkip)(void);
extern int (*dde_kit_initcall_6_init_crypto_wep)(void);
extern int (*dde_kit_initcall_6_init_ieee80211_acl)(void);
extern int (*dde_kit_initcall_6_init_ieee80211_xauth)(void);
extern int (*dde_kit_initcall_6_init_scanner_ap)(void);
extern int (*dde_kit_initcall_6_init_scanner_sta)(void);
extern int (*dde_kit_initcall_6_init_wlan)(void);


void do_initcalls(void)
{
	dde_kit_initcall_1_dde_linux26_page_cache_init();
	dde_kit_initcall_1_helper_init();
	dde_kit_initcall_2_pci_driver_init();
	dde_kit_initcall_2_pcibus_class_init();;
	dde_kit_initcall_4__call_init_workqueues();
	dde_kit_initcall_4_dde_linux26_init_pci();
	dde_kit_initcall_4_net_dev_init();
	dde_kit_initcall_4_wireless_nlevent_init();
	dde_kit_initcall_6_pci_init();
	dde_kit_initcall_6_init_wlan();
	dde_kit_initcall_6_init_scanner_sta();
	dde_kit_initcall_6_init_ath_hal();
	dde_kit_initcall_6_init_ath_rate_sample();
	dde_kit_initcall_6_init_ath_pci();
}


namespace Nic {

	static Nic::Mac_address _mac_addr;

	/* declaration of local handlers */
	static void dde_rx_handler(unsigned if_index,
	                           const unsigned char *packet,
	                           unsigned packet_len);
	static void dde_tx_handler(const char *essid);


	typedef Genode::Thread<8192> Tx_handler_thread;


	/*
	 * Nic-session component class
	 */
	class Session_component : public Session_rpc_object,
	                          public Tx_handler_thread
	{
		private:

			Genode::Dataspace_capability _tx_ds;         /* buffer for tx channel   */
			Genode::Dataspace_capability _rx_ds;         /* buffer for rx channel   */
			Genode::Lock                 _startup_lock;  /* signals ready to submit */
			const char                  *_essid;

		public:

			/**
			 * Constructor
			 *
			 * \param tx_buf_size        buffer size for tx channel
			 * \param rx_buf_size        buffer size for rx channel
			 * \param rx_block_alloc     rx block allocator
			 * \param ep                 entry point used for packet stream
			 */
			Session_component(Genode::size_t          tx_buf_size,
			                  Genode::size_t          rx_buf_size,
			                  Genode::Allocator_avl  *rx_block_alloc,
			                  const char             *essid,
			                  Genode::Rpc_entrypoint &ep)
			:
				Session_rpc_object(Genode::env()->ram_session()->alloc(tx_buf_size),
				                   Genode::env()->ram_session()->alloc(rx_buf_size),
				                   static_cast<Genode::Range_allocator *>(rx_block_alloc), ep),
				Tx_handler_thread("tx_handler"),
				_startup_lock(Genode::Lock::LOCKED), _essid(essid)
			{ }

			Mac_address  mac_address() { return _mac_addr;    }
			Tx::Sink*    tx_sink()     { return _tx.sink();   }
			Rx::Source*  rx_source()   { return _rx.source(); }

			/* thread entry function */
			void entry() { dde_tx_handler(_essid); }

			/* signal that nic driver is ready to handle packets */
			void ready() { _startup_lock.unlock(); }

			/* blocks until nic driver is ready to handle packets */
			void wait_for_completion() { _startup_lock.lock(); }
	};

	/*
	 * Nic-session component is a singleton,
	 * only one client should use the driver directly.
	 */
	static Session_component *_session = 0;

	/*
	 * Callback function, called whenever data was received by the card.
	 */
	static void dde_rx_handler(unsigned if_index,
	                           const unsigned char *packet,
	                           unsigned packet_len)
	{
		Session_component::Rx::Source *rx_source = _session->rx_source();
		try {
			/* flush remaining acknowledgements */
			while (rx_source->ack_avail())
				rx_source->release_packet(rx_source->get_acked_packet());

			/* allocate packet in rx channel */
			Packet_descriptor packet_to_client =
				rx_source->alloc_packet(packet_len);

			/* copy received data to rx packet and submit it to our client */
			Genode::memcpy(rx_source->packet_content(packet_to_client),
			               packet, packet_len);
			rx_source->submit_packet(packet_to_client);
		} catch (Session_component::Rx::Source::Packet_alloc_failed) {
			PWRN("transmit packet allocation failed, drop packet");
		}
	}

	/*
	 * Server loop, initializes DDE subsystem,
	 * waits for packets from the client and puts them into the card's buffer.
	 */
	static void dde_tx_handler(const char *essid)
	{
		using namespace Genode;

		PDBG("--- initcalls");
		dde_linux26_init();
		do_initcalls();

		PDBG("--- init NET");
		int cnt = dde_linux26_net_init();
		PDBG("    number of devices: %d", cnt);

		PDBG("--- init rx_callbacks");
		dde_linux26_net_register_rx_callback(dde_rx_handler);

		/* get nic index of atheros card */
		int idx = dde_linux26_wifi_atheros_idx();

		/* get mac address */
		dde_linux26_net_get_mac_addr(idx, (unsigned char*)_mac_addr.addr);

		/* set essid */
		PDBG("Set essid to %s", essid);
		dde_linux26_wifi_set_essid(idx, essid);

		/* we need to wait until the card can associate with the AP */
		Timer::Connection timer;
		timer.msleep(8000);

		/* signal that server is ready to handle requests from the client */
		_session->ready();

		Session_component::Tx::Sink *tx_sink = _session->tx_sink();

		/* server loop, handling send packets of the client */
		while (true) {
			Packet_descriptor packet_from_client;

			/* get packet, block until a packet is available */
			packet_from_client = tx_sink->get_packet();
			if (!packet_from_client.valid()) {
				PWRN("received invalid packet");
				continue;
			}
			size_t packet_size       = packet_from_client.size();
			const unsigned char *src = (unsigned char*)
				tx_sink->packet_content(packet_from_client);

			/* send it to the network buffer */
			if (dde_linux26_net_tx(idx, src, packet_size))
				PWRN("Sending packet failed!");

			/* acknowledge packet to the client */
			if (!tx_sink->ready_to_ack())
				PDBG("need to wait until ready-for-ack");
			tx_sink->acknowledge_packet(packet_from_client);
		}
	}

	/**
	 * Shortcut for single-client root component
	 */
	typedef Genode::Root_component<Session_component, Genode::Single_client> Root_component;

	/*
	 * Root component, handling new session requests.
	 */
	class Root : public Root_component
	{
		private:

			const char             *_essid;
			Genode::Rpc_entrypoint &_ep;

		protected:

			/*
			 * Always returns the singleton nic-session component.
			 */
			Session_component *_create_session(const char *args)
			{
				using namespace Genode;

				size_t ram_quota =
					Arg_string::find_arg(args, "ram_quota"  ).ulong_value(0);
				size_t tx_buf_size =
					Arg_string::find_arg(args, "tx_buf_size").ulong_value(0);
				size_t rx_buf_size =
					Arg_string::find_arg(args, "rx_buf_size").ulong_value(0);

				/* delete ram quota by the memory needed for the session */
				size_t session_size = max(4096U, sizeof(Session_component)
				                          + sizeof(Allocator_avl));
				if (ram_quota < session_size)
					throw Root::Quota_exceeded();

				/*
				 * Check if donated ram quota suffices for both
				 * communication buffers. Also check both sizes separately
				 * to handle a possible overflow of the sum of both sizes.
				 */
				if (tx_buf_size                  > ram_quota - session_size
					|| rx_buf_size               > ram_quota - session_size
					|| tx_buf_size + rx_buf_size > ram_quota - session_size) {
					PERR("insufficient 'ram_quota', got %zd, need %zd",
					     ram_quota, tx_buf_size + rx_buf_size + session_size);
					throw Root::Quota_exceeded();
				}

				/* allocator needed to allocate rx blocks */
				Allocator_avl *alloc =
					new (md_alloc()) Allocator_avl(env()->heap());
				Nic::_session =
					new (md_alloc()) Session_component(tx_buf_size,
				                                       rx_buf_size,
				                                       alloc, _essid, _ep);
				Nic::_session->start();
				Nic::_session->wait_for_completion();
				return Nic::_session;
			}

		public:

			Root(Genode::Rpc_entrypoint *session_ep,
			     Genode::Allocator      *md_alloc,
			     const char             *essid)
			: Root_component(session_ep, md_alloc), _essid(essid), _ep(*session_ep) { }
	};
}


/**
 * Get name of the ESSID.
 */
static void process_config(char *essid, Genode::size_t sz)
{
	using namespace Genode;

	Xml_node config_node = config()->xml_node();

	for (unsigned i = 0; i < config_node.num_sub_nodes(); ++i) {
		Xml_node file_node = config_node.sub_node(i);
		if (file_node.has_type("essid")) {
			file_node.value(essid, sz);
			break;
		}
	}
}


int main(int, char **)
{
	using namespace Genode;

	enum { STACK_SIZE = 4096 };
	static Cap_connection cap;
	static Rpc_entrypoint ep(&cap, STACK_SIZE, "nic_ep");

	char essid[64];
	process_config(essid, sizeof(essid));

	static Nic::Root nic_root(&ep, env()->heap(), essid);
	env()->parent()->announce(ep.manage(&nic_root));

	sleep_forever();
	return 0;
}
