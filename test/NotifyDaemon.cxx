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
	enum class State {
		Init = 0,
		Listen,
		ProcessEvents,
	};

	EventLoop event_loop;
	RootPool root_pool;
	Pg::AsyncConnection conn;
	std::string schema;
	State state = State::Init;

	NotifyDaemon(const char *conninfo, const char *schema_)
	  : conn(event_loop, conninfo, schema_, *this)
	  , schema(schema_)
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
		state = State::Listen;
		std::string sql("LISTEN \"");
		if (!schema.empty() && schema != "public") {
			sql += schema;
			sql += ':';
		}
		sql += "events_posted\"";
		conn.SendQuery(*this, sql.c_str());
	}

	void ProcessEvent()
	{
		std::string sql(R"(
		UPDATE events SET
		processed_at = NOW()
		WHERE id = (
			SELECT id FROM events
			WHERE processed_at IS NULL
			FOR UPDATE SKIP LOCKED
			LIMIT 1
		)
		RETURNING id;
		)");
		conn.SendQuery(*this, sql.c_str());
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
		ProcessEvent();
	}

	void OnError(std::exception_ptr e) noexcept override // AsyncConnectionHandler
	{
		PrintException(e);
	}

	void OnResult(Pg::Result &&result) override // AsyncResultHandler
	{
		fmt::print("result status: {}\n", result.GetStatus());
		if (state == State::ProcessEvents) {
			const auto event_id = result.GetLongValue(0, 0);
			fmt::print("event processed: {}\n", event_id);
		}
	}

	void OnResultEnd() override // AsyncResultHandler
	{
		if (state == State::Listen) {
			fmt::print("Started listening\n");
			state = State::ProcessEvents;
		}
	}
};

int
main(int argc, char **argv)
{
	try {
		if (argc != 2) {
			fmt::print(stderr, "Usage: {} CONNINFO\n", argv[0]);
			return EXIT_FAILURE;
		}
		const auto conninfo = argv[1];
		NotifyDaemon instance(conninfo, "");
		instance.conn.Connect();
		instance.event_loop.Run();
		return EXIT_SUCCESS;
	} catch (...) {
		PrintException(std::current_exception());
		return EXIT_FAILURE;
	}

	return 0;
}
