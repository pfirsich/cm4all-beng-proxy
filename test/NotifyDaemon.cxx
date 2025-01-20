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

class NotifyDaemon {
	Pg::Connection db;
	std::string schema;
	std::string datacenter_id;

	BengControl::Client control_client;

public:
	NotifyDaemon(const char *datacenter_id_,
		     const char *conninfo,
		     const char *schema_,
		     const char *control_server) noexcept
	  : db(conninfo)
	  , schema(schema_)
	  , datacenter_id(datacenter_id_)
	  , control_client(control_server)
	{
	}

	[[noreturn]] void Run();

private:
	void Listen();
	void WaitForNotify();
	std::tuple<long, std::string_view, std::string_view> GetEvent();
	void DeleteEvent(long event_id);
	bool ProcessEvent();
	void ProcessEvents();
};

[[noreturn]] void
NotifyDaemon::Run()
{
	Listen();
	ProcessEvents(); // Process all events after startup

	while (true) {
		try {
			WaitForNotify();
			ProcessEvents();
		} catch (const std::exception &exc) {
			fmt::print(stderr, "Exception: {}\n", exc.what());
		}
	}
}

void
NotifyDaemon::Listen()
{
	std::string sql("LISTEN \"");
	if (!schema.empty() && schema != "public") {
		sql += schema;
		sql += ':';
	}
	sql += "events_posted\"";
	db.ExecuteParams(sql.c_str());
}

void
NotifyDaemon::WaitForNotify()
{
	fmt::print("wait for notify\n");
	while (FileDescriptor(db.GetSocket()).WaitReadable(1000) == 0) {}

	Pg::Notify notify;
	do {
		db.ConsumeInput();
		notify = db.GetNextNotify();
	} while (notify);
}

std::tuple<long, std::string_view, std::string_view>
NotifyDaemon::GetEvent()
{
	fmt::print("get event\n");
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
	const auto res = db.ExecuteParams(sql, datacenter_id);

	if (!res.IsQuerySuccessful()) {
		throw std::runtime_error("Error processing event");
	}

	// We might be contending for the events with other daemons, so we might just "miss"
	if (res.GetAffectedRows() == 0) {
		return { 0, "", "" };
	}

	const auto event_id = res.GetLongValue(0, 0);
	const auto event = res.GetValueView(0, 1);
	const auto params = res.GetValueView(0, 2);
	return { event_id, event, params };
}

void
NotifyDaemon::DeleteEvent(long event_id)
{
	fmt::print("delete\n");
	const auto sql = "DELETE FROM events WHERE id = $1";
	const auto res = db.ExecuteParams(sql, event_id);

	if (!res.IsCommandSuccessful()) {
		throw std::runtime_error("Error deleting event");
	}
}

bool
NotifyDaemon::ProcessEvent()
{
	const auto [event_id, event, params] = GetEvent();
	if (event_id == 0 && event.empty()) {
		return false;
	}

	fmt::print("event: {}, {}, {}\n", event_id, event, params);
	auto [command, payload] = GetControlMessage(event, params);
	control_client.Send(command, payload);

	DeleteEvent(event_id);

	return true;
}

void
NotifyDaemon::ProcessEvents()
{
	while (ProcessEvent()) {}
}

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
