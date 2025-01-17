#include "event/Loop.hxx"
#include "io/SpliceSupport.hxx"
#include "memory/fb_pool.hxx"
#include "net/RConnectSocket.hxx"
#include "net/SocketError.hxx"
#include "net/UniqueSocketDescriptor.hxx"
#include "net/control/Padding.hxx"
#include "net/control/Protocol.hxx"
#include "pg/AsyncConnection.hxx"
#include "pool/RootPool.hxx"
#include "pool/pool.hxx"
#include "util/ByteOrder.hxx"
#include "util/PrintException.hxx"

#include <fmt/core.h>

#include <queue>

#include <sys/socket.h>

struct NotifyDaemon
  : Pg::AsyncConnectionHandler
  , Pg::AsyncResultHandler {
	enum class CurrentQuery {
		None = 0,
		Listen,
		ProcessEvent,
		DeleteEvent,
	};

	struct Event {
		long id;
		std::string event;
		std::string params;
	};

	EventLoop event_loop;
	RootPool root_pool;

	Pg::AsyncConnection db;
	std::string schema;
	std::string datacenter_id;
	CurrentQuery current_query = {};

	// I can't really use BengControl::Client, because I need MSG_DONTWAIT and I need to handle EAGAIN and I can't
	// add this to BengControl::Client in a clean, backwards compatible way.
	UniqueSocketDescriptor control_socket;
	SocketEvent control_socket_event;

	std::queue<Event> event_queue;
	std::queue<long> delete_events;
	bool notified = false;
	bool initial_flush = true;

	NotifyDaemon(std::string datacenter_id_,
		     const char *conninfo,
		     const char *schema_,
		     const char *control_server) noexcept
	  : db(event_loop, conninfo, schema_, *this)
	  , schema(schema_)
	  , datacenter_id(std::move(datacenter_id_))
	  , control_socket(ResolveConnectDatagramSocket(control_server, BengControl::DEFAULT_PORT))
	  , control_socket_event(event_loop, BIND_THIS_METHOD(ControlSocketWritable), control_socket)
	{
		db.Connect();
		control_socket_event.Schedule(EpollEvents::WRITE);
	}

	static std::pair<BengControl::Command, std::string> GetControlMessage(const Event &)
	{
		// TODO: Proper mapping
		return { BengControl::Command::TCACHE_INVALIDATE, "" };
	}

	void SendControlMessages() noexcept
	{
		while (!event_queue.empty()) {
			const auto &event = event_queue.front();
			fmt::print("> notify: {}, {}\n", event.id, event.event);
			auto [cmd, payload] = GetControlMessage(event);

			uint32_t magic = ToBE32(BengControl::MAGIC);
			BengControl::Header header{ ToBE16(payload.size()), ToBE16(uint16_t(cmd)) };
			static uint8_t padding[3] = { 0, 0, 0 };

			struct iovec iov[] = {
				{ &magic, sizeof(magic) },
				{ &header, sizeof(header) },
				{ payload.data(), payload.size() },
				{ padding, BengControl::PaddingSize(payload.size()) },
			};
			msghdr msg{ nullptr, 0, iov, 4, nullptr, 0, 0 };

			const auto res = control_socket.Send(msg, MSG_DONTWAIT);
			if (res < 0) {
				if (errno == ENETUNREACH) {
					// TODO: see libcommon/src/net/control/Client.cxx
					fmt::print("sendmsg failed: ENETUNREACH\n");
				} else if (errno == EAGAIN) {
					// Just try again when the socket becomes writable again
					break;
				} else {
					// TODO. I think there is nothing we can do in the general case.
					fmt::print("sendmsg failed: {}\n", errno);
					std::exit(1);
				}
			}

			assert(res > 0);
			delete_events.push(event.id);
			SendNextQuery();
			event_queue.pop();
		}
	}

	void ControlSocketWritable(unsigned events) noexcept
	{
		if (events & SocketEvent::WRITE) {
			SendControlMessages();
		}
	}

	void Listen()
	{
		std::string sql("LISTEN \"");
		if (!schema.empty() && schema != "public") {
			sql += schema;
			sql += ':';
		}
		sql += "events_posted\"";
		current_query = CurrentQuery::Listen;
		db.SendQuery(*this, sql.c_str());
	}

	void ProcessEvent()
	{
		notified = false;
		const auto sql = R"(
		UPDATE events SET
		processed_at = NOW()
		WHERE id = (
			SELECT id FROM events
			WHERE datacenter_id = $1 AND (processed_at IS NULL OR NOW() - processed_at >= INTERVAL '5 min')
			FOR UPDATE SKIP LOCKED
			LIMIT 1
		)
		RETURNING id, event, params;
		)";
		current_query = CurrentQuery::ProcessEvent;
		db.SendQuery(*this, sql, datacenter_id);
	}

	void DeleteEvent(long event_id)
	{
		const auto sql = "DELETE FROM events WHERE id = $1";
		current_query = CurrentQuery::DeleteEvent;
		db.SendQuery(*this, sql, event_id);
	}

	void SendNextQuery()
	{
		if (current_query != CurrentQuery::None) {
			return;
		}
		if (!delete_events.empty()) {
			const auto id = delete_events.front();
			delete_events.pop();
			DeleteEvent(id);
		} else if (notified || initial_flush) {
			ProcessEvent();
		}
	}

	void OnConnect() override // AsyncConnectionHandler
	{
		fmt::print("connected\n");
		Listen();
	}

	void OnDisconnect() noexcept override // AsyncConnectionHandler
	{
		fmt::print("disconnected\n");
	}

	void OnNotify(const char *name) override // AsyncConnectionHandler
	{
		fmt::print("notify: {}\n", name);
		notified = true;
		SendNextQuery();
	}

	void OnError(std::exception_ptr e) noexcept override // AsyncConnectionHandler
	{
		PrintException(e);
	}

	void OnResult(Pg::Result &&result) override // AsyncResultHandler
	{
		fmt::print("result status: {}\n", fmt::underlying(result.GetStatus()));
		if (!result.IsCommandSuccessful() && !result.IsQuerySuccessful()) {
			return;
		}
		if (current_query == CurrentQuery::ProcessEvent) {
			// We might be contending for the events with other daemons, so we might just "miss"
			if (result.GetAffectedRows()) {
				const auto event_id = result.GetLongValue(0, 0);
				const auto event = result.GetValueView(0, 1);
				const auto params = result.GetValueView(0, 2);
				// We can't just do the notification here, because it might finish before this query is
				// done (OnResultEnd was called) and we must not call SendQuery before the current Query
				// is finished.
				event_queue.emplace(Event{ event_id, std::string(event), std::string(params) });
				SendControlMessages();
			} else {
				fmt::print("Updated 0 rows\n");
				initial_flush = false;
			}
		}
	}

	void OnResultEnd() override // AsyncResultHandler
	{
		if (current_query == CurrentQuery::Listen) {
			fmt::print("Started listening\n");
		} else if (current_query == CurrentQuery::ProcessEvent) {
			fmt::print("Processed event\n");
		} else if (current_query == CurrentQuery::DeleteEvent) {
			fmt::print("Completed event\n");
		}
		current_query = CurrentQuery::None;
		SendNextQuery();
	}
};

int
main(int argc, char **argv)
{
	try {
		if (argc != 4) {
			fmt::print(stderr, "Usage: {} DATACENTERID PGCONNINFO CONTROLSERVER\n", argv[0]);
			return EXIT_FAILURE;
		}
		const auto datacenter_id = argv[1];
		const auto conninfo = argv[2];
		const auto control_server = argv[3];
		NotifyDaemon instance(datacenter_id, conninfo, "", control_server);
		instance.event_loop.Run();
		return EXIT_SUCCESS;
	} catch (...) {
		PrintException(std::current_exception());
		return EXIT_FAILURE;
	}

	return 0;
}
