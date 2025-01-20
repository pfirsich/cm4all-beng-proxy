#include "event/Loop.hxx"
#include "net/control/Client.hxx"
#include "pg/AsyncConnection.hxx"
#include "util/PrintException.hxx"

#include <fmt/core.h>

#include <queue>

static std::pair<BengControl::Command, std::string>
GetControlMessage(std::string_view /*events*/, std::string_view /*params*/)
{
	// TODO: Proper mapping
	return { BengControl::Command::TCACHE_INVALIDATE, "" };
}

struct NotifyDaemon
  : Pg::AsyncConnectionHandler
  , Pg::AsyncResultHandler {
	enum class CurrentQuery {
		None = 0,
		Listen,
		ProcessEvent,
		DeleteEvent,
	};

	EventLoop event_loop;

	Pg::AsyncConnection db;
	std::string schema;
	std::string datacenter_id;
	CurrentQuery current_query = {};

	BengControl::Client control_client;

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
	  , control_client(control_server)
	{
		db.Connect();
	}

	void Run() { event_loop.Run(); }

	void SendControlMessage(long event_id, std::string_view event, std::string_view params)
	{
		auto [command, payload] = GetControlMessage(event, params);
		control_client.Send(command, payload);
		delete_events.push(event_id);
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
				SendControlMessage(event_id, event, params);
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
		instance.Run();
		return EXIT_SUCCESS;
	} catch (...) {
		PrintException(std::current_exception());
		return EXIT_FAILURE;
	}

	return 0;
}
