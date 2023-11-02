// SPDX-License-Identifier: BSD-2-Clause
// Copyright CM4all GmbH
// author: Max Kellermann <mk@cm4all.com>

#pragma once

#include <exception>
#include <forward_list>
#include <string>

struct AcmeOrder {
	std::string location;

	enum class Status {
		PENDING,
		READY,
		PROCESSING,
		VALID,
		INVALID,
	} status;

	std::forward_list<std::string> authorizations;
	std::string finalize;
	std::string certificate;

	static Status ParseStatus(const std::string_view s);
	static const char *FormatStatus(Status s) noexcept;
};
