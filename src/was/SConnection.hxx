// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#pragma once

#include "IdleConnection.hxx"
#include "stock/Item.hxx"
#include "io/Logger.hxx"

class WasStockConnection
	: public StockItem, WasIdleConnectionHandler
{
	LLogger logger;

	WasIdleConnection connection;

public:
	explicit WasStockConnection(CreateStockItem c) noexcept;

	WasStockConnection(CreateStockItem c, WasSocket &&_socket) noexcept
		:WasStockConnection(c)
	{
		Open(std::move(_socket));
	}

	auto &GetEventLoop() const noexcept {
		return connection.GetEventLoop();
	}

	const auto &GetSocket() const noexcept {
		return connection.GetSocket();
	}

	/**
	 * Set the "stopping" flag.  Call this after sending
	 * #WAS_COMMAND_STOP, before calling hstock_put().  This will
	 * make the stock wait for #WAS_COMMAND_PREMATURE.
	 */
	void Stop(uint64_t _received) noexcept;

	virtual void SetSite([[maybe_unused]] const char *site) noexcept {}
	virtual void SetUri([[maybe_unused]] const char *uri) noexcept {}

protected:
	void Open(WasSocket &&_socket) noexcept {
		connection.Open(std::move(_socket));
	}

	/* virtual methods from class StockItem */
	bool Borrow() noexcept override;
	bool Release() noexcept override;

private:
	/* virtual methods from class WasIdleConnectionHandler */
	void OnWasIdleConnectionClean() noexcept override;
	void OnWasIdleConnectionError(std::exception_ptr e) noexcept override;
};
