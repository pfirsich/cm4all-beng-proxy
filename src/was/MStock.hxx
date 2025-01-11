// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#pragma once

#include "spawn/ChildStock.hxx"
#include "stock/MultiStock.hxx"
#include "pool/Ptr.hxx"

#include <span>

class AllocatorPtr;
struct ChildOptions;
class StockItem;
struct WasSocket;
class SocketDescriptor;
class EventLoop;
class SpawnService;

class MultiWasStock final : MultiStockClass, ChildStockClass {
	PoolPtr pool;
	ChildStock child_stock;
	MultiStock mchild_stock;

public:
	MultiWasStock(unsigned limit, unsigned max_idle,
		      EventLoop &event_loop, SpawnService &spawn_service,
		      Net::Log::Sink *log_sink,
		      const ChildErrorLogOptions &log_options) noexcept;

	auto &GetEventLoop() const noexcept {
		return mchild_stock.GetEventLoop();
	}

	std::size_t DiscardSome() noexcept {
		return mchild_stock.DiscardOldestIdle(64);
	}

	void FadeAll() noexcept {
		mchild_stock.FadeAll();
	}

	void FadeTag(std::string_view tag) noexcept;

	/**
	 * The resulting #StockItem will be a #WasStockConnection
	 * instance.
	 */
	void Get(AllocatorPtr alloc,
		 const ChildOptions &options,
		 const char *executable_path,
		 std::span<const char *const> args,
		 unsigned parallelism, unsigned concurrency,
		 StockGetHandler &handler,
		 CancellablePointer &cancel_ptr) noexcept;

private:
	/* virtual methods from class MultiStockClass */
	std::size_t GetLimit(const void *request,
			     std::size_t _limit) const noexcept override;
	Event::Duration GetClearInterval(const void *info) const noexcept override;
	StockItem *Create(CreateStockItem c, StockItem &shared_item) override;

	/* virtual methods from class ChildStockClass */
	StockRequest PreserveRequest(StockRequest request) noexcept override;
	bool WantStderrPond(const void *info) const noexcept override;
	std::string_view GetChildTag(const void *info) const noexcept override;
	std::unique_ptr<ChildStockItem> CreateChild(CreateStockItem c,
						    const void *info,
						    ChildStock &child_stock) override;
	void PrepareChild(const void *info, PreparedChildProcess &p,
			  FdHolder &close_fds) override;
};
