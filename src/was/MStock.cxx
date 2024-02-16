// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#include "MStock.hxx"
#include "SConnection.hxx"
#include "was/async/Socket.hxx"
#include "was/async/MultiClient.hxx"
#include "cgi/Address.hxx"
#include "stock/Stock.hxx"
#include "stock/MapStock.hxx"
#include "pool/DisposablePointer.hxx"
#include "pool/tpool.hxx"
#include "AllocatorPtr.hxx"
#include "cgi/ChildParams.hxx"
#include "spawn/ChildStockItem.hxx"
#include "spawn/Prepared.hxx"
#include "event/SocketEvent.hxx"
#include "net/SocketPair.hxx"
#include "net/UniqueSocketDescriptor.hxx"
#include "system/Error.hxx"
#include "util/Exception.hxx"
#include "util/StringList.hxx"

#include <cassert>
#include <optional>

class MultiWasChild final : public ChildStockItem, Was::MultiClientHandler {
	EventLoop &event_loop;

	std::optional<Was::MultiClient> client;

public:
	MultiWasChild(CreateStockItem c,
		      ChildStock &_child_stock,
		      std::string_view _tag) noexcept
		:ChildStockItem(c, _child_stock, _tag),
		 event_loop(c.stock.GetEventLoop())
	{}

	WasSocket Connect() {
		return client->Connect();
	}

protected:
	/* virtual methods from class ChildStockItem */
	void Prepare(ChildStockClass &cls, void *info,
		     PreparedChildProcess &p) override;

private:
	/* virtual methods from class Was::MultiClientHandler */
	void OnMultiClientDisconnect() noexcept override {
		client.reset();
		Disconnected();
	}

	void OnMultiClientError(std::exception_ptr error) noexcept override {
		(void)error; // TODO log error?
		client.reset();
		Disconnected();
	}
};

void
MultiWasChild::Prepare(ChildStockClass &cls, void *info,
		       PreparedChildProcess &p)
{
	assert(!client);

	ChildStockItem::Prepare(cls, info, p);

	auto [for_child, for_parent] = CreateSocketPair(SOCK_SEQPACKET);

	p.SetStdin(std::move(for_child));

	Was::MultiClientHandler &client_handler = *this;
	client.emplace(event_loop, std::move(for_parent), client_handler);
}

class MultiWasConnection final
	: public WasStockConnection
{
	MultiWasChild &child;

public:
	MultiWasConnection(CreateStockItem c, MultiWasChild &_child)
		:WasStockConnection(c, _child.Connect()),
		 child(_child) {}

	[[gnu::pure]]
	std::string_view GetTag() const noexcept {
		return child.GetTag();
	}

	void SetSite(const char *site) noexcept override {
		child.SetSite(site);
	}

	void SetUri(const char *uri) noexcept override {
		child.SetUri(uri);
	}
};

MultiWasStock::MultiWasStock(unsigned limit, [[maybe_unused]] unsigned max_idle,
			     EventLoop &event_loop, SpawnService &spawn_service,
			     SocketDescriptor log_socket,
			     const ChildErrorLogOptions &log_options) noexcept
	:child_stock(spawn_service,
		     nullptr, // TODO do we need ListenStreamSpawnStock here?
		     *this,
		     log_socket, log_options),
	 mchild_stock(event_loop, child_stock,
		      limit,
		      // TODO max_idle,
		      *this) {}

std::size_t
MultiWasStock::GetLimit(const void *request,
			std::size_t _limit) const noexcept
{
	const auto &params = *(const CgiChildParams *)request;

	if (params.parallelism > 0)
		return params.parallelism;

	return _limit;
}

Event::Duration
MultiWasStock::GetClearInterval(const void *info) const noexcept
{
	const auto &params = *(const CgiChildParams *)info;

	return params.options.ns.mount.pivot_root == nullptr
		? std::chrono::minutes(15)
		/* lower clear_interval for jailed (per-account?)
		   processes */
		: std::chrono::minutes(5);
}

bool
MultiWasStock::WantStderrPond(void *info) const noexcept
{
	const auto &params = *(const CgiChildParams *)info;
	return params.options.stderr_pond;
}

std::string_view
MultiWasStock::GetChildTag(void *info) const noexcept
{
	const auto &params = *(const CgiChildParams *)info;

	return params.options.tag;
}

std::unique_ptr<ChildStockItem>
MultiWasStock::CreateChild(CreateStockItem c,
			   void *info,
			   ChildStock &_child_stock)
{
	return std::make_unique<MultiWasChild>(c, _child_stock,
					       GetChildTag(info));
}

void
MultiWasStock::PrepareChild(void *info, PreparedChildProcess &p)
{
	const auto &params = *(const CgiChildParams *)info;

	p.Append(params.executable_path);
	for (auto i : params.args)
		p.Append(i);

	params.options.CopyTo(p);
}

StockItem *
MultiWasStock::Create(CreateStockItem c, StockItem &shared_item)
{
	auto &child = (MultiWasChild &)shared_item;

	return new MultiWasConnection(c, child);
}

void
MultiWasStock::FadeTag(std::string_view tag) noexcept
{
	mchild_stock.FadeIf([tag](const StockItem &item){
		const auto &child = (const MultiWasChild &)item;
		return child.IsTag(tag);
	});
}

void
MultiWasStock::Get(AllocatorPtr alloc,
		   const ChildOptions &options,
		   const char *executable_path,
		   std::span<const char *const> args,
		   unsigned parallelism, unsigned concurrency,
		   StockGetHandler &handler,
		   CancellablePointer &cancel_ptr) noexcept
{
	const TempPoolLease tpool;

	auto r = NewDisposablePointer<CgiChildParams>(alloc, executable_path,
						      args, options,
						      parallelism, concurrency,
						      false);
	const char *key = r->GetStockKey(*tpool);

	mchild_stock.Get(key, std::move(r), concurrency, handler, cancel_ptr);
}
