// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#include "SConnection.hxx"
#include "stock/Stock.hxx"

#include <cassert>

WasStockConnection::WasStockConnection(CreateStockItem c) noexcept
	:StockItem(c),
	 logger(GetStockName()),
	 connection(c.stock.GetEventLoop(), *this) {}

void
WasStockConnection::Stop(uint64_t _received) noexcept
{
	assert(!is_idle);

	connection.Stop(_received);
}

bool
WasStockConnection::Borrow() noexcept
{
	return connection.Borrow();
}

bool
WasStockConnection::Release() noexcept
{
	connection.Release();
	unclean = connection.IsStopping();
	return true;
}

void
WasStockConnection::OnWasIdleConnectionClean() noexcept
{
	ClearUncleanFlag();
}

void
WasStockConnection::OnWasIdleConnectionError(std::exception_ptr e) noexcept
{
	logger(2, e);
	InvokeIdleDisconnect();
}
