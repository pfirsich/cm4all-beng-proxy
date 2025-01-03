#include <queue>

#include <fmt/core.h>

#include <event/Loop.hxx>
#include <io/SpliceSupport.hxx>
#include <memory/fb_pool.hxx>
#include <pg/AsyncConnection.hxx>
#include <pool/RootPool.hxx>
#include <pool/pool.hxx>
#include <util/PrintException.hxx>

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
	Pg::AsyncConnection conn;
	std::string schema;
	std::string datacenter_id;
	CurrentQuery current_query = {};
	std::queue<Event> event_queue;
	bool notified = false;
	bool initial_flush = true;

	NotifyDaemon(const char *conninfo, const char *schema_, std::string datacenter_id_)
	  : conn(event_loop, conninfo, schema_, *this)
	  , schema(schema_)
	  , datacenter_id(std::move(datacenter_id_))
	{
		// PInstance
#ifndef NDEBUG
		event_loop.SetPostCallback(BIND_FUNCTION(pool_commit));
#endif
		// TestInstance
		direct_global_init();
		fb_pool_init();
	}

	~NotifyDaemon()
	{
		fb_pool_deinit(); // TestInstance
		pool_commit();	  // AutoPoolCommit
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
		conn.SendQuery(*this, sql.c_str());
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
		conn.SendQuery(*this, sql, datacenter_id);
	}

	void Notify(const Event &event)
	{
		fmt::print("notify: {}, {}\n", event.id, event.event);

		// TODO: Actually send a notification

		// Now completed, remove the event (alternatively set completed_at)
		const auto sql = "DELETE FROM events WHERE id = $1";
		current_query = CurrentQuery::DeleteEvent;
		conn.SendQuery(*this, sql, event.id);
	}

	void Query()
	{
		if (current_query != CurrentQuery::None) {
			return;
		}
		if (!event_queue.empty()) {
			const auto event = event_queue.front();
			event_queue.pop();
			Notify(event);
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
		// TODO: Put this event in a queue or something, because we shouldn't be calling ProcessEvent here,
		// because we might already be processing another query in which case we must not call SendQuery again.
		fmt::print("notify: {}\n", name);
		notified = true;
		Query();
	}

	void OnError(std::exception_ptr e) noexcept override // AsyncConnectionHandler
	{
		PrintException(e);
	}

	void OnResult(Pg::Result &&result) override // AsyncResultHandler
	{
		fmt::print("result status: {}\n", result.GetStatus());
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
		Query();
	}
};

int
main(int argc, char **argv)
{
	try {
		if (argc != 3) {
			fmt::print(stderr, "Usage: {} CONNINFO DATACENTER\n", argv[0]);
			return EXIT_FAILURE;
		}
		const auto conninfo = argv[1];
		const auto datacenter_id = argv[2];
		NotifyDaemon instance(conninfo, "", datacenter_id);
		instance.conn.Connect();
		instance.event_loop.Run();
		return EXIT_SUCCESS;
	} catch (...) {
		PrintException(std::current_exception());
		return EXIT_FAILURE;
	}

	return 0;
}
